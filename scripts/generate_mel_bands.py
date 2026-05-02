#!/usr/bin/env python3
"""Generate the MEL_BANDS[] table that lives in SharedSpectralAnalysis.cpp.

The firmware's mel filterbank is hand-baked into a static const array.
v33's regression (CLAUDE.md "No Silent Fallbacks" reference incident)
came from a size-mismatched MEL_BANDS array that aggregate-init zero-
filled the missing entries. This script avoids that by generating the
full table for any (n_mels, fmin, fmax, n_fft, sample_rate) combo
exactly the way librosa does, so train- and inference-time mel filters
match.

Usage:
    python3 scripts/generate_mel_bands.py --n-mels 80 --fmin 30 --fmax 14000 \
        --n-fft 512 --sr 31250 > /tmp/mel_bands_v36.cpp.fragment

The output is the C++ array literal ready to paste into
SharedSpectralAnalysis.cpp, plus a header comment matching the librosa
parity assertion already in that file.
"""
from __future__ import annotations

import argparse
import sys

import numpy as np

try:
    import librosa
except ImportError:
    print("librosa required: pip install librosa", file=sys.stderr)
    sys.exit(1)


def generate_mel_bands(
    n_mels: int, fmin: float, fmax: float, n_fft: int, sr: int
) -> list[tuple[int, int, int, float]]:
    """Return per-band (startBin, centerBin, endBin, centerHz) matching firmware MelBandDef.

    The firmware's MelBandDef stores three uint8_t bin indices (start, center, end)
    and reconstructs triangular weights at runtime — no per-bin weight storage.
    htk=True, norm=None matches the existing firmware comment + librosa parity.

    centerHz is included for the human-readable comment after each row.
    """
    # The firmware's MelBandDef encodes the TRIANGLE VERTICES (where weight = 0
    # at startBin/endBin, weight = 1 at centerBin). librosa.filters.mel returns
    # the weights themselves; the boundary bins (weight = 0) are implicit and
    # excluded from `where(weights > 0)`. So reading bin indices off the weight
    # matrix gives "first nonzero" — one bin INSIDE the triangle vs the firmware's
    # "edge of the triangle". We need the edge.
    #
    # Use mel-frequency boundaries directly: librosa's mel filterbank places its
    # n_mels+2 boundary points at evenly-spaced mel frequencies, and the i-th
    # filter triangle has vertices at boundaries[i] / [i+1] / [i+2]. Quantize
    # those frequencies to FFT bin indices.
    mel_freqs = librosa.mel_frequencies(n_mels=n_mels + 2, fmin=fmin, fmax=fmax, htk=True)
    bin_hz = sr / n_fft
    bands: list[tuple[int, int, int, float]] = []
    empty_count = 0
    for i in range(n_mels):
        # round() matches numpy's bankers-rounding which is what librosa uses
        # internally when it discretizes mel boundaries onto FFT bins.
        start = int(round(mel_freqs[i] / bin_hz))
        center = int(round(mel_freqs[i + 1] / bin_hz))
        end = int(round(mel_freqs[i + 2] / bin_hz))
        # If the band is fully collapsed (start>=end after rounding), the
        # firmware's triangle reconstruction yields weightSum==0 → boot
        # assert. Catch here. Note: center==start (with end>start) and
        # center==end (with start<end) are NOT degenerate — the firmware
        # special-cases the zero-width ramp by emitting weight=1.0 for the
        # peak bin, so weightSum > 0. Verified on v36's {0,1,1}/{1,1,2}
        # pattern bands which produce weightSum=1.0 at boot. PR 138 round-3
        # bot review claimed these were degenerate — bot's analysis was
        # wrong; tracing the firmware bin loop confirms.
        # Monotonicity invariant the firmware relies on. mel_frequencies()
        # output is strictly monotone, so this should always hold; assert
        # explicitly so a future change to the binning math (or a librosa
        # behavior change) trips here at generation time rather than
        # producing a silently malformed table that fails at firmware boot.
        # MUST run BEFORE the degenerate-band check: a non-monotone triplet
        # like (3, 5, 4) has end > start (so passes degenerate) but center
        # > end (violates invariant). Per PR 138 rounds 5+6 review.
        assert start <= center <= end, (
            f"band {i}: non-monotone vertices ({start}, {center}, {end}) — "
            f"firmware triangle reconstruction assumes start <= center <= end"
        )
        if end <= start:
            # Degenerate (zero-width triangle) → firmware weightSum==0 →
            # boot assert. Fail at generation time instead. The bands list
            # is discarded after the empty_count > 0 sys.exit below; we
            # don't need to append a placeholder here.
            empty_count += 1
            continue
        center_hz = center * bin_hz
        bands.append((start, center, end, center_hz))

    if empty_count > 0:
        print(
            f"WARNING: {empty_count} of {n_mels} mel bands have zero filter response. "
            f"This means n_mels is too high for n_fft at this fmin/fmax — increase n_fft or "
            f"decrease n_mels. The firmware will fail loudly at boot via the weightSum==0 "
            f"BLINKY_ASSERT in SharedSpectralAnalysis.cpp.",
            file=sys.stderr,
        )
        sys.exit(2)

    # Sanity check: every bin index emitted to the firmware MEL_BANDS table
    # must fit in uint8_t (max value 255). The earlier "n_bins <= 256" check
    # was wrong on two counts: (a) a 512-point FFT produces 257 unique
    # real-valued bins (0..256, including DC and Nyquist), and (b) for
    # configs where fmax approaches Nyquist the end bin index *can* be 256
    # — which silently wraps to 0 when assigned to uint8_t in firmware,
    # exactly the v33-class regression this script was written to prevent.
    # Validate the actual emitted per-band max_end_bin instead. Per PR 138
    # round-2 review (claude bot HIGH).
    max_end_bin = max(end for _, _, end, _ in bands)
    if max_end_bin > 255:
        print(
            f"ERROR: band end bin {max_end_bin} > 255 — exceeds uint8_t "
            f"MelBandDef.endBin limit. This would silently wrap to "
            f"{max_end_bin & 0xFF} in firmware. Either lower fmax (current "
            f"config has n_fft={n_fft} giving {n_fft//2 + 1} usable bins) or "
            f"widen MelBandDef in SharedSpectralAnalysis.h to uint16_t.",
            file=sys.stderr,
        )
        sys.exit(3)

    return bands


def emit_cpp(
    bands: list[tuple[int, int, int, float]],
    n_mels: int, fmin: float, fmax: float, n_fft: int, sr: int,
) -> str:
    """Format the bands as the C++ array literal for SharedSpectralAnalysis.cpp."""
    lines = [
        f"// Generated by scripts/generate_mel_bands.py "
        f"(n_mels={n_mels}, fmin={fmin}, fmax={fmax}, n_fft={n_fft}, sr={sr}, htk=True, norm=None).",
        "// MUST stay in sync with NUM_MEL_BANDS / MEL_MIN_FREQ / MEL_MAX_FREQ /",
        "// FFT_SIZE / SAMPLE_RATE in SharedSpectralAnalysis.h. When pasting this",
        "// table, also add a static_assert checking those five constants match",
        "// the values above so a future config change can't desync silently",
        "// (v33's size-mismatch zero-fill regression is the canonical example).",
        "static const MelBandDef MEL_BANDS[SpectralConstants::NUM_MEL_BANDS] = {",
    ]
    for i, (start, center, end, center_hz) in enumerate(bands):
        lines.append(
            f"    {{ {start:>3}, {center:>3}, {end:>3} }},  // {i:>2}: {center_hz:>6.0f} Hz center"
        )
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--n-mels", type=int, required=True)
    p.add_argument("--fmin", type=float, required=True)
    p.add_argument("--fmax", type=float, required=True)
    p.add_argument("--n-fft", type=int, required=True)
    p.add_argument("--sr", type=int, required=True)
    p.add_argument("--output", type=str, default=None,
                   help="Write to file instead of stdout. Failures (disk full, "
                        "permission, parent dir missing) raise an exception "
                        "rather than silently producing an empty file from "
                        "shell redirection. Per PR 138 round-7 review.")
    args = p.parse_args()

    bands = generate_mel_bands(args.n_mels, args.fmin, args.fmax, args.n_fft, args.sr)
    cpp = emit_cpp(bands, args.n_mels, args.fmin, args.fmax, args.n_fft, args.sr)
    if args.output:
        # Open in 'x' mode would refuse to overwrite — for safety against
        # accidental clobbers the operator must explicitly delete the
        # previous output. But that's stricter than typical workflow; use
        # 'w' here and warn loudly when an overwrite is happening so a
        # surprised "wait, did I just clobber the live table?" is at least
        # surfaced. Per PR 138 round-9 review.
        import os
        if os.path.exists(args.output):
            print(
                f"WARNING: overwriting existing {args.output} "
                f"(previous content lost)",
                file=sys.stderr,
            )
        with open(args.output, "w") as f:
            f.write(cpp)
            f.write("\n")
        print(f"Wrote {args.n_mels}-band table to {args.output}", file=sys.stderr)
    else:
        print(cpp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
