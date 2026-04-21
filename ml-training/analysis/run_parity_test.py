"""Drive the native C++ parity harness and assert per-feature MAE.

Flow:
  1. Build tests/parity/parity_harness via CMake if it's missing or stale.
  2. Load the audio file, run compute_stft() → (n_frames, N_FFT/2) magnitudes,
     prepend a DC bin of zeros (DC is skipped by features.py but the firmware
     still stores it in preWhitenMagnitudes_[0]), write the binary file in
     the format parity_harness reads.
  3. Invoke the harness, capture its per-frame CSV.
  4. Compute the same features in Python via compute_all_features() on the
     same audio (using the same DC-skipped magnitudes).
  5. Per-feature MAE assertion; exit non-zero if any feature drifts.

Run `./venv/bin/python -m analysis.run_parity_test --help` for CLI.
"""

from __future__ import annotations

import argparse
import csv
import logging
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import librosa
import numpy as np

_ROOT = Path(__file__).resolve().parents[1]
if str(_ROOT) not in sys.path:
    sys.path.insert(0, str(_ROOT))

from analysis.features import (  # noqa: E402
    HOP,
    N_FFT,
    SR,
    crest_factor,
    high_frequency_content,
    raw_superflux,
    spectral_centroid,
    spectral_flatness,
    spectral_rolloff,
)

log = logging.getLogger("parity")

# Match the firmware's MAE target for mel pipeline parity (≈0.44 INT8 levels
# on a 256-level scale = 0.0017 normalized units). The shape features here
# are not normalized to [0,1], so we use relative tolerance: absolute MAE
# must be < 1e-4 OR < 1e-4 × feature range, whichever is larger.
ABS_MAE_TOL = 1e-4
REL_MAE_TOL = 1e-4


def build_harness(build_dir: Path, force: bool) -> Path:
    harness = build_dir / "parity_harness"
    if harness.exists() and not force:
        return harness
    build_dir.mkdir(parents=True, exist_ok=True)
    if not (build_dir / "CMakeCache.txt").exists() or force:
        log.info("Configuring cmake in %s", build_dir)
        subprocess.run(["cmake", ".."], cwd=build_dir, check=True)
    log.info("Building parity_harness")
    subprocess.run(
        ["cmake", "--build", ".", "-j"], cwd=build_dir, check=True
    )
    if not harness.exists():
        raise RuntimeError(f"Harness build succeeded but {harness} is missing")
    return harness


def firmware_layout_mags(audio: np.ndarray) -> np.ndarray:
    """Compute magnitudes in firmware's bin layout.

    Firmware `NUM_BINS = FFT_SIZE / 2 = 128` stores DC at bin 0 and positive
    frequencies at bins 1..127. It discards the Nyquist bin (rfft index 128).
    The feature loops skip bin 0 (DC) and run over i=1..127.

    `compute_stft` in features.py drops DC but keeps Nyquist, returning 128
    values. For bit-for-bit parity we need to match firmware exactly: DC at
    index 0 (zeroed; firmware computes non-zero DC but never reads it), and
    bins 1..127 populated from rfft magnitudes at the same indices. Nyquist
    from rfft is ignored.
    """
    n_frames = max(0, (len(audio) - N_FFT) // HOP + 1)
    if n_frames == 0:
        return np.zeros((0, 128), dtype=np.float32)
    starts = np.arange(n_frames) * HOP
    idx = starts[:, None] + np.arange(N_FFT)
    frames = audio[idx] * np.hamming(N_FFT).astype(np.float32)
    spectra = np.fft.rfft(frames, axis=1)  # (n_frames, 129): bins 0..128
    out = np.zeros((n_frames, 128), dtype=np.float32)
    out[:, 1:] = np.abs(spectra[:, 1:128])  # bin 1..127 → firmware bins 1..127
    # out[:, 0] = DC is left at 0; firmware stores np.abs(spectra[:, 0]) but
    # the feature loops skip it, so zero is equivalent.
    return out


def write_mags_bin(audio: np.ndarray, out_path: Path) -> tuple[int, int]:
    mags = firmware_layout_mags(audio)
    n_frames, num_bins = mags.shape
    with out_path.open("wb") as f:
        f.write(struct.pack("<ii", num_bins, n_frames))
        mags.astype("<f4").tofile(f)
    return num_bins, n_frames


def python_features_from_firmware_layout(mags_firmware: np.ndarray) -> dict[str, np.ndarray]:
    """Run feature functions on firmware-layout magnitudes (128 bins, DC=0).

    features.py expects DC-skipped input of size 127. Slice off DC and pass
    bins 1..127 — Python feature math matches firmware exactly (same bin
    indexing in loops, same bin range).
    """
    sliced = mags_firmware[:, 1:]  # (n_frames, 127) — firmware bins 1..127
    return {
        "centroid": spectral_centroid(sliced).astype(np.float64),
        "crest": crest_factor(sliced).astype(np.float64),
        "rolloff": spectral_rolloff(sliced).astype(np.float64),
        "hfc": high_frequency_content(sliced).astype(np.float64),
        "flatness": spectral_flatness(sliced).astype(np.float64),
        # raw_superflux needs prev-frame state internally (leading frame = 0),
        # which the function handles by returning n_frames values with out[0]=0.
        "raw_flux": raw_superflux(sliced).astype(np.float64),
    }


def run_harness(harness: Path, bin_path: Path, csv_path: Path) -> None:
    result = subprocess.run(
        [str(harness), str(bin_path), str(csv_path)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        log.error("harness stderr: %s", result.stderr)
        raise RuntimeError(f"parity_harness exited {result.returncode}")
    log.info("harness stderr: %s", result.stderr.strip())


def load_harness_csv(csv_path: Path) -> dict[str, np.ndarray]:
    # Features covered by the parity comparison. Mel bands (mel0..melN-1)
    # are also written by the harness but are verified by a separate mel
    # parity tool (`ml-training/scripts/validate_features.py`), not here.
    compared = ["centroid", "crest", "rolloff", "hfc", "flatness", "raw_flux"]
    cols: dict[str, list[float]] = {k: [] for k in compared}
    with csv_path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            for k in compared:
                cols[k].append(float(row[k]))
    return {k: np.asarray(v, dtype=np.float64) for k, v in cols.items()}


# `python_features_from_firmware_layout` above replaces the naive path that
# read compute_stft directly — the layouts differ by one bin and that matters.


def compare(
    harness: dict[str, np.ndarray], python: dict[str, np.ndarray]
) -> dict[str, dict[str, float]]:
    report: dict[str, dict[str, float]] = {}
    for name in sorted(harness):
        h = harness[name]
        p = python[name]
        n = min(len(h), len(p))
        if n == 0:
            continue
        h = h[:n]
        p = p[:n]
        diff = h - p
        mae = float(np.mean(np.abs(diff)))
        max_err = float(np.max(np.abs(diff)))
        # np.ptp was removed as an array method in numpy 2.0 and is being
        # deprecated as a free function; use explicit max-min for stability.
        p_range = float(np.max(p) - np.min(p))
        value_range = p_range if p_range > 0 else 1.0
        rel_mae = mae / max(value_range, 1e-9)
        report[name] = {
            "mae": mae,
            "max_err": max_err,
            "rel_mae": rel_mae,
            "value_range": value_range,
            "frames": n,
        }
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audio", required=True, type=Path, help="WAV or MP3 input")
    parser.add_argument(
        "--harness",
        type=Path,
        default=None,
        help="Path to prebuilt parity_harness. Default: build it under tests/parity/build.",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=_ROOT.parent / "tests" / "parity" / "build",
    )
    parser.add_argument("--force-rebuild", action="store_true")
    parser.add_argument("--keep-temp", action="store_true")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    harness_path = args.harness or build_harness(args.build_dir, args.force_rebuild)

    audio, _ = librosa.load(str(args.audio), sr=SR, mono=True)
    # Use the same RMS normalization as run_catalog.py so features are on
    # the same scale whether this test is run standalone or as part of the
    # bigger pipeline.
    rms = np.sqrt(np.mean(audio**2) + 1e-10)
    audio = audio * (10 ** (-35.0 / 20) / rms)

    tmp_dir = _ROOT / "outputs" / "parity"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    bin_path = tmp_dir / f"{args.audio.stem}.mags.bin"
    csv_path = tmp_dir / f"{args.audio.stem}.harness.csv"

    log.info("Writing magnitudes → %s", bin_path)
    num_bins, n_frames = write_mags_bin(audio, bin_path)
    log.info("Frames: %d, bins (incl DC): %d", n_frames, num_bins)

    log.info("Running harness → %s", csv_path)
    run_harness(harness_path, bin_path, csv_path)

    harness_feats = load_harness_csv(csv_path)
    # Reuse the exact same magnitudes the harness saw — reading back from
    # bin_path would also work, but this avoids the disk round-trip.
    mags_fw = firmware_layout_mags(audio)
    python_feats = python_features_from_firmware_layout(mags_fw)
    report = compare(harness_feats, python_feats)

    print(f"\n{'feature':10s} {'frames':>7s} {'range':>10s} {'MAE':>12s} {'max_err':>12s} {'rel_MAE':>10s}  pass?")
    print("-" * 72)
    failures: list[str] = []
    for name, r in report.items():
        ok = (r["mae"] < ABS_MAE_TOL) or (r["rel_mae"] < REL_MAE_TOL)
        tag = "OK" if ok else "FAIL"
        if not ok:
            failures.append(name)
        print(
            f"{name:10s} {r['frames']:>7d} {r['value_range']:>10.3g}"
            f" {r['mae']:>12.2e} {r['max_err']:>12.2e} {r['rel_mae']:>10.2e}  {tag}"
        )

    if not args.keep_temp:
        for p in (bin_path, csv_path):
            p.unlink(missing_ok=True)

    if failures:
        log.error("Parity FAILED for: %s", ", ".join(failures))
        return 1
    log.info("Parity OK on all features.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
