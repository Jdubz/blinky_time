"""The firmware's mel front end, built from its spec (#295).

The NN runs on the firmware's `SharedSpectralAnalysis::getRawMelBands()`, which reads the
integer table `MEL_BANDS[]`, not an HTK filterbank. Training must build the same features,
so the table and the arithmetic come from one spec, `ml-training/spec/mel_frontend_v1.json`.
`analysis/test_mel_parity.py` checks the spec against the firmware source, and the
arithmetic here against the firmware itself on golden frames.
"""

from __future__ import annotations

import hashlib
import json
from functools import lru_cache
from pathlib import Path

import numpy as np

SPEC_PATH = Path(__file__).resolve().parents[1] / "spec" / "mel_frontend_v1.json"


@lru_cache(maxsize=None)
def load_spec(path: str | None = None) -> dict:
    with open(path or SPEC_PATH, encoding="utf-8") as f:
        spec = json.load(f)
    if len(spec["bands"]) != spec["n_mels"]:
        raise ValueError(f"{path or SPEC_PATH}: {len(spec['bands'])} bands, n_mels {spec['n_mels']}")
    return spec


def band_weights(start: int, center: int, end: int, num_bins: int) -> dict[int, float]:
    """One band's per-bin weights, exactly as `computeMelBandsFrom` builds them."""
    w: dict[int, float] = {}
    for b in range(start, min(center, num_bins - 1) + 1):
        w[b] = (b - start) / (center - start) if center > start else 1.0
    for b in range(center + 1, min(end, num_bins - 1) + 1):
        w[b] = 1.0 - (b - center) / (end - center) if end > center else 1.0
    return w


def filterbank(spec: dict | None = None, n_freqs: int | None = None) -> np.ndarray:
    """(n_mels, n_freqs) weights, each row divided by its weight sum, so `fb @ mags` is the
    firmware's weighted average. `n_freqs` defaults to n_fft//2 + 1 (the rFFT's bins); columns
    from `num_bins` on (the Nyquist bin) are zero, as the firmware never reads them."""
    spec = spec or load_spec()
    n_freqs = n_freqs or spec["n_fft"] // 2 + 1
    fb = np.zeros((spec["n_mels"], n_freqs), dtype=np.float64)
    for m, (start, center, end) in enumerate(spec["bands"]):
        w = band_weights(start, center, end, spec["num_bins"])
        total = sum(w.values())
        if total <= 0:
            # The firmware asserts the same thing at begin(): a band that reads nothing is a
            # corrupt table, never a band to fill with zeros.
            raise ValueError(f"mel band {m} {[start, center, end]} has zero weight")
        for b, v in w.items():
            fb[m, b] = v / total
    return fb


def log_map(band_energy: np.ndarray, spec: dict | None = None) -> np.ndarray:
    """Band energies -> the NN's [0, 1] input: exact 0 below the silence threshold, else
    the firmware's log map."""
    spec = spec or load_spec()
    e = np.asarray(band_energy, dtype=np.float64)
    db = 10.0 * np.log10(e + spec["log_epsilon"])
    out = np.clip((db + spec["db_range"]) / spec["db_range"], 0.0, 1.0)
    return np.where(e < spec["silence_threshold"], 0.0, out)


def mel_from_magnitudes(mags: np.ndarray, spec: dict | None = None) -> np.ndarray:
    """(n_frames, n_freqs) magnitudes -> (n_frames, n_mels) firmware mel."""
    spec = spec or load_spec()
    fb = filterbank(spec, mags.shape[1])
    return log_map(mags.astype(np.float64) @ fb.T, spec)


# ---- Which front end a dataset and a model were made with (#291) ----
#
# The firmware runs one front end, `SpectralConstants::MEL_FRONTEND_VERSION`, and refuses at
# build time a model header that records any other. The record starts where the features are
# made (prepare_dataset.py writes it into the processed dir), and train.py and
# export_tflite.py carry it through to the header, so a model trained by code we don't run
# can still be checked against what the device feeds it.

RECORD_FILE = "mel_frontend.json"
# The header's number for the librosa HTK filterbank every model through v36 trained on.
# Not a firmware front end: the build accepts it only for a model the firmware waives by hash.
LIBROSA_HTK_VERSION = 0


def spec_digest(spec: dict) -> str:
    """sha256 of the spec's content: every key but the `_`-prefixed notes, so editing a comment
    doesn't orphan every prepared dataset, while changing a band or a constant does."""
    content = {k: v for k, v in spec.items() if not k.startswith("_")}
    return hashlib.sha256(json.dumps(content, sort_keys=True).encode()).hexdigest()


def frontend_record(kind: str) -> dict:
    """The record for features built with `audio.mel_filterbank: kind`."""
    if kind == "firmware_table":
        spec = load_spec()
        return {"mel_filterbank": kind, "version": spec["version"], "spec_sha256": spec_digest(spec)}
    if kind == "librosa_htk":
        return {"mel_filterbank": kind, "version": LIBROSA_HTK_VERSION}
    raise ValueError(f"unknown mel_filterbank {kind!r}")


def check_record(record: object, where: str) -> dict:
    """`record` if it is one `frontend_record` could have made, else a ValueError. A firmware
    table record must name this checkout's spec (its version and content digest)."""
    if not isinstance(record, dict) or record.get("mel_filterbank") not in ("firmware_table",
                                                                           "librosa_htk"):
        raise ValueError(f"{where}: not a mel front-end record: {record!r}")
    want = frontend_record(record["mel_filterbank"])
    if record != want:
        raise ValueError(f"{where}: front end {record} is not this checkout's {want}. The "
                         f"features were made from a different spec; re-run prepare_dataset.py.")
    return record


def read_record(directory: Path) -> dict:
    """The record in `directory` (a processed dataset or a training output), refused loudly
    if it is missing or doesn't match this checkout."""
    path = Path(directory) / RECORD_FILE
    if not path.exists():
        raise FileNotFoundError(
            f"{path} is missing, so nothing says which mel front end these features were made "
            f"with (#291). Re-run prepare_dataset.py, which writes it.")
    return check_record(json.loads(path.read_text(encoding="utf-8")), str(path))


def write_record(directory: Path, record: dict) -> Path:
    path = Path(directory) / RECORD_FILE
    path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def model_record(model_path: Path, in_checkpoint: dict | None, stated: str | None) -> dict:
    """The front end of the model at `model_path`, for export: from the checkpoint, from the
    record train.py wrote beside it, and from `stated` (export's --mel-frontend, for a model
    trained before records existed). Every source given must agree, and at least one must."""
    found = []
    if in_checkpoint is not None:
        found.append(("the checkpoint", check_record(in_checkpoint, str(model_path))))
    if (Path(model_path).parent / RECORD_FILE).exists():
        found.append((str(Path(model_path).parent / RECORD_FILE),
                      read_record(Path(model_path).parent)))
    if stated is not None:
        found.append(("--mel-frontend", frontend_record(stated)))
    if not found:
        raise ValueError(
            f"nothing records which mel front end {model_path} was trained on: no record in "
            f"the checkpoint or beside it ({RECORD_FILE}). For a model trained before records "
            f"existed, state it with --mel-frontend; every model through v36 is librosa_htk.")
    for where, record in found[1:]:
        if record != found[0][1]:
            raise ValueError(f"{where} says {record}, but {found[0][0]} says {found[0][1]}")
    return found[0][1]
