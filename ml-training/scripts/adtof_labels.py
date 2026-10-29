"""Kick/snare labels from ADTOF, a published drum transcription model (#284).

ADTOF (Zehren et al., https://github.com/MZehren/ADTOF) is a CRNN trained on 359 h of real
music, transcribing five classes at 100 frames/s. It replaces the band-onset generator because
it classifies a hit by what the instrument is, not by which frequency band fired first. On the
drum label suite (drum_label_suite.py; the numbers in drum_label_adtof.json) it beats the band
generator on every row: no snare labeled a kick (against 106 of 120), timing on edm/ at the
onset consensus (against +26 ms late), and almost no phantom snares (against 14.6 %).

This runs the original: its own code, TensorFlow model and weights, pinned to one commit. Its
repository's content, code and weights alike, is licensed CC BY-NC-SA 4.0 (its README and
LICENSE): non-commercial use only, which blinky_time is, with attribution, and share-alike for
adapted material. Labels made with it carry that attribution in their `generator` field. It is
an installed dependency (the Spark's `adtof` runtime, my-spark #1561; CI's adtof job), never
copied into this repo. It is not the PyTorch port, which has no licence.

ADTOF labels the audio it is given: a full mix or a drum stem. Kick (GM 35), snare (38) and
hi-hat (42, weight 0, kept so the schema matches the band generator's) are written; toms and
cymbals are dropped. A kick and a snare at the same instant are both kept: unlike the band
generator, there is no cross-band leakage to merge away.
"""

from __future__ import annotations

import hashlib
from functools import lru_cache
from pathlib import Path
from typing import Any

ADTOF_COMMIT = "b3968fb332f69b65ee07c089fc62f436503755db"
ADTOF_PACKAGE = f"ADTOF @ git+https://github.com/MZehren/ADTOF@{ADTOF_COMMIT}"
MODEL = {"modelName": "Frame_RNN", "scenario": "adtofAll", "fold": 0}
# The checkpoint the package ships (adtof/models/), pinned by git blob sha at ADTOF_COMMIT, so
# they are checkable against GitHub without downloading anything.
WEIGHT_BLOBS = {
    "Frame_RNN_adtofAll_0.index": "c835b33d1981a6bff634b8137bb4d546fb9ded74",
    "Frame_RNN_adtofAll_0.data-00000-of-00001": "38df829b7590315306cc3ad3923cb9b085a9f400",
}
# Frame_RNN's tuned per-class peak-picking thresholds, in ADTOF's class order
# (config.LABELS_5: BD 35, SD 38, TT 47, HH 42, CY+RD 49).
LABELS_5 = [35, 38, 47, 42, 49]
THRESHOLDS = [0.22, 0.24, 0.32, 0.22, 0.30]
FPS = 100
CLASSES = {35: ("kick", 1.0), 38: ("snare", 1.0), 42: ("hihat", 0.0)}
MIN_RMS = 0.005  # as the band generator: below it, a stem is a failed separation
RMS_SR = 16000

GENERATOR: dict[str, Any] = {
    "algorithm": "adtof_frame_rnn",
    # v1 was the unlicensed PyTorch port; v2 is the original. Their labels never mix.
    "version": 2,
    "params": {
        "package": ADTOF_PACKAGE,
        "model": MODEL,
        "weight_blobs": WEIGHT_BLOBS,
        "thresholds": THRESHOLDS,
        "fps": FPS,
        "classes": {str(k): v[0] for k, v in CLASSES.items()},
        "weights": {v[0]: v[1] for v in CLASSES.values()},
        "min_rms": MIN_RMS,
    },
    "attribution": "ADTOF, Zehren et al. (github.com/MZehren/ADTOF), CC BY-NC-SA 4.0",
}


def git_blob_sha(path: Path) -> str:
    """The sha git (and GitHub's contents API) gives a file: sha1 of `blob <size>\\0<bytes>`."""
    data = path.read_bytes()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


@lru_cache(maxsize=1)
def _model() -> tuple[Any, dict]:
    """Load once per process. Refuse a checkpoint that isn't the pinned one, thresholds that
    aren't Frame_RNN's, and a partial load: ADTOF's `load_weights` doesn't check that every
    variable was restored, and a half-loaded model labels plausibly and wrongly."""
    from adtof import config
    from adtof.model.model import Model

    for name, want in WEIGHT_BLOBS.items():
        path = Path(config.CHECKPOINT_DIR) / name
        if not path.is_file():
            raise RuntimeError(f"ADTOF checkpoint {path} is missing; install {ADTOF_PACKAGE}")
        got = git_blob_sha(path)
        if got != want:
            raise RuntimeError(f"ADTOF checkpoint {name} is blob {got[:12]}, not the pinned "
                               f"{want[:12]}; install {ADTOF_PACKAGE}")
    model, hparams = Model.modelFactory(**MODEL)
    if not model.weightLoadedFlag:
        raise RuntimeError(f"ADTOF loaded no weights from {config.CHECKPOINT_DIR}")
    model.model.load_weights(model.path).assert_existing_objects_matched()
    if list(config.LABELS_5) != LABELS_5 or list(hparams["labels"]) != LABELS_5:
        raise RuntimeError(f"ADTOF's classes {hparams['labels']} != {LABELS_5}")
    if [round(t, 6) for t in hparams["peakThreshold"]] != THRESHOLDS:
        raise RuntimeError(f"ADTOF's thresholds {hparams['peakThreshold']} != {THRESHOLDS}")
    if hparams["sampleRate"] != FPS:
        raise RuntimeError(f"ADTOF's frame rate {hparams['sampleRate']} != {FPS}")
    return model, hparams


def label_file(path: Path) -> dict[str, Any]:
    """The labels for one audio file, in the kick_weighted schema."""
    import librosa
    import numpy as np

    y, _ = librosa.load(str(path), sr=RMS_SR, mono=True)
    rms = float(np.sqrt(np.mean(y**2))) if len(y) else 0.0
    base = {"source": GENERATOR["algorithm"], "rms": rms, "weights": GENERATOR["params"]["weights"],
            "generator": GENERATOR}
    if rms < MIN_RMS:
        return {**base, "onsets": [], "kick_count": 0, "snare_count": 0, "hihat_count": 0,
                "total": 0, "skipped": True,
                "skip_reason": f"silent audio (RMS={rms:.6f} < {MIN_RMS})"}

    # What ADTOF's Model.predictFolder does per track, minus its catch-all: it logs an
    # exception and moves on, which would leave a track silently unlabeled.
    from adtof.model.peakPicking import PeakPicking
    from adtof.model.track import Track

    model, hparams = _model()
    act = model.predict(Track(str(path), **hparams), **hparams)
    pp = PeakPicking()
    pp.setParameters(**hparams)
    peaks = pp.predict([act], **hparams)[0]
    onsets = sorted(
        ({"time": round(float(t), 4), "weight": CLASSES[p][1], "type": CLASSES[p][0]}
         for p, times in peaks.items() if p in CLASSES for t in times),
        key=lambda o: (o["time"], o["type"]),
    )
    counts = {k: sum(1 for o in onsets if o["type"] == k) for k in ("kick", "snare", "hihat")}
    return {**base, "onsets": onsets, "kick_count": counts["kick"],
            "snare_count": counts["snare"], "hihat_count": counts["hihat"], "total": len(onsets)}
