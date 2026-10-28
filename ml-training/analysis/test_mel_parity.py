"""Mel parity: training's front end against the firmware's (#295).

Three checks, cheapest first:
  1. spec/mel_frontend_v1.json carries the firmware's MEL_BANDS[] and constants, read from
     blinky-things/audio/SharedSpectralAnalysis.{cpp,h};
  2. scripts/mel_frontend.py (what training uses) reproduces spec/mel_golden_v1.npz, mel the
     firmware computed on golden magnitude frames: numpy only, no compiler;
  3. with --harness, the firmware itself (tests/parity/parity_harness) still produces those
     golden values, so a change on either side fails here.

Usage (from ml-training/):
  python -m analysis.test_mel_parity                       # 1 and 2
  python -m analysis.test_mel_parity --harness PATH        # 1, 2 and 3 (CI)
  python -m analysis.test_mel_parity --harness PATH --regenerate   # rewrite the golden file
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ML = Path(__file__).resolve().parents[1]
REPO = ML.parent
sys.path.insert(0, str(ML))

from scripts.mel_frontend import filterbank, load_spec, mel_from_magnitudes  # noqa: E402

CPP = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.cpp"
HEADER = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.h"
GOLDEN = ML / "spec" / "mel_golden_v1.npz"

# Python is float64, the firmware float32; the [0, 1] outputs agree to ~1e-6.
ATOL = 2e-5


def firmware_bands() -> list[list[int]]:
    body = re.search(r"static const MelBandDef MEL_BANDS\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                     CPP.read_text(), re.S)
    if body is None:
        raise AssertionError(f"MEL_BANDS[] not found in {CPP}")
    return [[int(a), int(b), int(c)]
            for a, b, c in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", body.group(1))]


def header_constant(name: str) -> float:
    text = HEADER.read_text()
    if name == "NUM_BINS":  # `NUM_BINS = FFT_SIZE / 2`
        assert re.search(r"constexpr int NUM_BINS = FFT_SIZE / 2;", text), "NUM_BINS changed"
        return header_constant("FFT_SIZE") / 2
    m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*([0-9.]+)f?\s*;", text)
    if m is None:
        raise AssertionError(f"{name} not found in {HEADER}")
    return float(m.group(1))


def check_spec_matches_firmware(spec: dict) -> None:
    fw = firmware_bands()
    assert fw == spec["bands"], (
        f"spec bands differ from {CPP.name} MEL_BANDS[] at "
        f"{[i for i, (a, b) in enumerate(zip(fw, spec['bands'])) if a != b] or 'length'}")
    for key, name in (("n_mels", "NUM_MEL_BANDS"), ("fmin", "MEL_MIN_FREQ"),
                      ("fmax", "MEL_MAX_FREQ"), ("db_range", "MEL_DB_RANGE"),
                      ("n_fft", "FFT_SIZE"), ("num_bins", "NUM_BINS")):
        assert float(spec[key]) == header_constant(name), f"spec {key} != firmware {name}"
    cpp = CPP.read_text()
    for value, what in ((spec["silence_threshold"], "silenceThreshold"),
                        (spec["log_epsilon"], "epsilon")):
        m = re.search(rf"const float {what} = ([0-9.e+-]+)f;", cpp)
        assert m and float(m.group(1)) == value, f"spec {what} != firmware"
    print(f"ok  spec matches the firmware's MEL_BANDS[] ({len(fw)} bands) and constants")


def golden_frames(num_bins: int) -> np.ndarray:
    """Deterministic magnitude frames that pin each part of the front end."""
    frames = [np.zeros(num_bins)]
    # 9.5e-7: below the 1e-6 silence floor, yet above where the log map alone reaches 0.
    for level in (5e-7, 9.5e-7, 3e-6, 1e-4, 1e-2, 1.0, 10.0, 100.0):  # silence .. clipping
        frames.append(np.full(num_bins, level))
    eye = np.eye(num_bins)  # one bin at a time: each band's exact support and weights
    frames.extend(eye[b] * 0.5 for b in range(num_bins))
    rng = np.random.default_rng(295)
    frames.extend(10.0 ** rng.uniform(-6, 1, size=(24, num_bins)))
    # A kick-like frame and a noise burst, through the firmware's window and FFT size.
    n = 2 * (num_bins)
    t = np.arange(n) / 16000
    window = 0.54 - 0.46 * np.cos(2 * np.pi * np.arange(n) / (n - 1))
    for sig in (0.8 * np.sin(2 * np.pi * 55 * t) * np.exp(-t / 0.05),
                0.3 * rng.standard_normal(n)):
        frames.append(np.abs(np.fft.rfft(sig * window))[:num_bins])
    return np.asarray(frames, dtype=np.float32)


def run_harness(harness: Path, mags: np.ndarray) -> np.ndarray:
    with tempfile.TemporaryDirectory() as d:
        inp, out = Path(d) / "mags.bin", Path(d) / "out.csv"
        with open(inp, "wb") as f:
            np.array([mags.shape[1], mags.shape[0]], dtype=np.int32).tofile(f)
            mags.astype(np.float32).tofile(f)
        r = subprocess.run([str(harness), str(inp), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"{harness} exited {r.returncode}: {r.stderr.strip()[-500:]}")
        header = out.read_text().splitlines()[0].split(",")
        cols = [i for i, h in enumerate(header) if h.startswith("mel")]
        data = np.loadtxt(out, delimiter=",", skiprows=1, ndmin=2)
    return data[:, cols].astype(np.float32)


def check_python_matches_golden(spec: dict) -> None:
    g = np.load(GOLDEN)
    got = mel_from_magnitudes(g["mags"], spec)
    err = np.abs(got - g["mel"])
    worst = np.unravel_index(int(err.argmax()), err.shape)
    assert err.max() <= ATOL, (
        f"training's mel differs from the firmware's by {err.max():.3g} "
        f"(frame {worst[0]}, band {worst[1]}): python {got[worst]:.6f}, firmware {g['mel'][worst]:.6f}")
    print(f"ok  python matches the firmware on {len(g['mags'])} golden frames "
          f"(max |diff| {err.max():.2g})")


def check_firmware_matches_golden(harness: Path) -> None:
    g = np.load(GOLDEN)
    fw = run_harness(harness, g["mags"])
    err = np.abs(fw - g["mel"])
    assert err.max() <= 1e-6, (
        f"the firmware's mel no longer matches {GOLDEN.name} (max |diff| {err.max():.3g}). "
        f"If that change is intended, update spec/mel_frontend_v1.json and regenerate the "
        f"golden file with --regenerate.")
    print(f"ok  the firmware still produces {GOLDEN.name}")


def check_librosa_is_not_the_firmware(spec: dict) -> None:
    """The librosa HTK filterbank must stay measurably different, or the golden frames have
    stopped testing what #295 found."""
    g = np.load(GOLDEN)
    try:
        import librosa
    except ImportError:
        print("skip the librosa comparison (librosa not installed)")
        return
    fb = librosa.filters.mel(sr=spec["sample_rate"], n_fft=spec["n_fft"], n_mels=spec["n_mels"],
                             fmin=spec["fmin"], fmax=spec["fmax"], htk=True, norm=None)
    fb = fb / np.maximum(fb.sum(axis=1, keepdims=True), 1e-10)
    old = np.clip((10 * np.log10(g["mags"] @ fb[:, :spec["num_bins"]].T + spec["log_epsilon"])
                   + spec["db_range"]) / spec["db_range"], 0, 1)
    assert np.abs(old - g["mel"]).max() > 0.05, "golden frames no longer separate librosa from the table"
    print(f"ok  the golden frames separate the old librosa filterbank "
          f"(max |diff| {np.abs(old - g['mel']).max():.2f})")


def check_configs() -> None:
    """Training's configs pick the firmware table by default (base.yaml), and a config whose
    mel settings the table can't serve is refused, never silently given the librosa bank."""
    from scripts.audio import check_mel_filterbank_cfg, load_config
    cfgs = ML / "configs"
    assert check_mel_filterbank_cfg(load_config(cfgs / "conv1d_w16_onset_v33_mel_only.yaml")) \
        == "firmware_table"
    try:
        check_mel_filterbank_cfg(load_config(cfgs / "conv1d_w16_onset_v30_mel_only.yaml"))
    except ValueError as e:
        assert "librosa_htk" in str(e)
    else:
        raise AssertionError("a 30-band config was given the 50-band firmware table")
    print("ok  configs default to the firmware table; incompatible mel settings are refused")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--harness", type=Path, help="tests/parity build's parity_harness")
    ap.add_argument("--regenerate", action="store_true", help="rewrite the golden file")
    a = ap.parse_args(argv)
    spec = load_spec()
    check_spec_matches_firmware(spec)
    assert np.allclose(filterbank(spec).sum(axis=1), 1.0)
    if a.regenerate:
        if not a.harness:
            ap.error("--regenerate needs --harness: the golden values come from the firmware")
        mags = golden_frames(spec["num_bins"])
        np.savez_compressed(GOLDEN, mags=mags, mel=run_harness(a.harness, mags))
        print(f"wrote {GOLDEN} ({len(mags)} frames)")
    if a.harness:
        check_firmware_matches_golden(a.harness)
    check_python_matches_golden(spec)
    check_librosa_is_not_the_firmware(spec)
    check_configs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
