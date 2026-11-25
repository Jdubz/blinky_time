"""Mel parity: training's front end against the mel front-end contract (#295).

The contract is spec/mel_frontend_v1.json and the golden vectors it names (`_golden`,
spec/mel_golden_v1.npz: magnitude frames and the mel the firmware computed on them).
blinky-firmware carries an identical copy and checks its own side (its MEL_BANDS[], constants,
MEL_FRONTEND_VERSION, and that its SharedSpectralAnalysis.cpp still produces the golden mel:
tests/parity/test_mel_contract.py). This repo reads no firmware source; it checks, numpy only:
  1. the contract is whole: the golden file is the one the spec names (sha256), shaped as the
     spec says, and the spec's file name is its version;
  2. scripts/mel_frontend.py (what training uses) reproduces the golden mel within the
     spec's tolerance;
  3. the golden frames still separate the old librosa HTK filterbank, and training's configs
     default to the table.

Usage (from ml-training/):
  python -m analysis.test_mel_parity

A front-end change starts in blinky-firmware (new version, regenerated vectors); then copy its
tests/parity/spec/ files here.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import numpy as np

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))

from scripts.mel_frontend import SPEC_PATH, filterbank, load_spec, mel_from_magnitudes  # noqa: E402


def golden_path(spec: dict) -> Path:
    return SPEC_PATH.parent / spec["_golden"]["file"]


def check_contract(spec: dict) -> None:
    g = spec["_golden"]
    assert SPEC_PATH.name == f"mel_frontend_v{spec['version']}.json", \
        f"{SPEC_PATH.name} holds version {spec['version']}"
    assert g["file"] == f"mel_golden_v{spec['version']}.npz", \
        f"{g['file']} for version {spec['version']}"
    assert spec["num_bins"] == spec["n_fft"] // 2, "num_bins is n_fft/2 (the Nyquist bin dropped)"
    assert all(0 <= a <= b <= c for a, b, c in spec["bands"]), "bands are [start, center, end]"
    path = golden_path(spec)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == g["sha256"], (
        f"{path.name} hashes to {digest}, the spec names {g['sha256']}: copy both files from "
        f"blinky-firmware tests/parity/spec/ together")
    data = np.load(path)
    assert data["mags"].shape == (g["frames"], spec["num_bins"]), data["mags"].shape
    assert data["mel"].shape == (g["frames"], spec["n_mels"]), data["mel"].shape
    print(f"ok  the contract is whole: {SPEC_PATH.name} ({len(spec['bands'])} bands) and "
          f"{path.name} ({g['frames']} frames, sha256 {digest[:12]})")


def check_python_matches_golden(spec: dict) -> None:
    g = np.load(golden_path(spec))
    got = mel_from_magnitudes(g["mags"], spec)
    err = np.abs(got - g["mel"])
    worst = np.unravel_index(int(err.argmax()), err.shape)
    assert err.max() <= spec["_golden"]["atol"], (
        f"training's mel differs from the firmware's by {err.max():.3g} "
        f"(frame {worst[0]}, band {worst[1]}): python {got[worst]:.6f}, firmware {g['mel'][worst]:.6f}")
    print(f"ok  python matches the firmware on {len(g['mags'])} golden frames "
          f"(max |diff| {err.max():.2g})")


def check_librosa_is_not_the_firmware(spec: dict) -> None:
    """The librosa HTK filterbank must stay measurably different, or the golden frames have
    stopped testing what #295 found."""
    g = np.load(golden_path(spec))
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
    argparse.ArgumentParser(description=__doc__.split("\n\n")[0]).parse_args(argv)
    spec = load_spec()
    check_contract(spec)
    assert np.allclose(filterbank(spec).sum(axis=1), 1.0)
    check_python_matches_golden(spec)
    check_librosa_is_not_the_firmware(spec)
    check_configs()
    return 0


if __name__ == "__main__":
    sys.exit(main())
