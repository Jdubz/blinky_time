"""Kick/snare labels from ADTOF, a published drum transcription model (#284).

ADTOF (Zehren et al., https://github.com/MZehren/ADTOF) is a CRNN trained on 359 h of real
music, transcribing five classes at 100 frames/s. It replaces the band-onset generator because
it classifies a hit by what the instrument is, not by which frequency band fired first. On the
drum label suite (drum_label_suite.py) it scores:
- synthetic kick F1 0.93 and snare F1 1.00, against 0.66 and 0.16 for the band generator;
- 0 snares labeled kicks, against 106 of 120;
- edm/ timing 0 ms against consensus, against +26 ms;
- edm/ phantom snares 0.2 %, against 14.6 %.

It runs through ADTOF-pytorch (https://github.com/xavriley/ADTOF-pytorch), a PyTorch port with
the weights bundled. That port has no licence file, so it is an installed dependency, pinned
below, and never copied into this repo. ADTOF's model and weights are licensed CC BY-NC-SA 4.0:
non-commercial use only, which blinky_time is, with attribution, and share-alike for adapted
material. Labels made with it carry that attribution in their `generator` field.

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

ADTOF_PYTORCH = "adtof-pytorch @ git+https://github.com/xavriley/ADTOF-pytorch@85c192e78f716ea0b111cc8a5ee4a8f6a3a4f8a9"
WEIGHTS_SHA256 = "1bc986e596ec47ba0b44916f87cd4a39f0b2bec23596df3fb5d0e87749217320"
# The port's tuned per-class peak-picking thresholds, in its class order (BD, SD, TT, HH, CY).
THRESHOLDS = [0.22, 0.24, 0.32, 0.22, 0.30]
FPS = 100
CLASSES = {35: ("kick", 1.0), 38: ("snare", 1.0), 42: ("hihat", 0.0)}
MIN_RMS = 0.005  # as the band generator: below it, a stem is a failed separation
RMS_SR = 16000

GENERATOR: dict[str, Any] = {
    "algorithm": "adtof_frame_rnn",
    "version": 1,
    "params": {
        "package": ADTOF_PYTORCH,
        "weights_sha256": WEIGHTS_SHA256,
        "thresholds": THRESHOLDS,
        "fps": FPS,
        "classes": {str(k): v[0] for k, v in CLASSES.items()},
        "weights": {v[0]: v[1] for v in CLASSES.values()},
        "min_rms": MIN_RMS,
    },
    "attribution": "ADTOF, Zehren et al. (github.com/MZehren/ADTOF), CC BY-NC-SA 4.0",
}


@lru_cache(maxsize=1)
def _model() -> Any:
    """Load once per process. Refuse weights that aren't the pinned ones, and refuse a partial
    load: the port defaults to strict=False, which would label with a half-loaded model."""
    from adtof_pytorch import (
        FRAME_RNN_THRESHOLDS,
        calculate_n_bins,
        create_frame_rnn_model,
        get_default_weights_path,
        load_pytorch_weights,
    )

    path = get_default_weights_path()
    if path is None:
        raise RuntimeError("adtof_pytorch ships no weights")
    got = hashlib.sha256(Path(path).read_bytes()).hexdigest()
    if got != WEIGHTS_SHA256:
        raise RuntimeError(f"ADTOF weights {got[:12]} are not the pinned {WEIGHTS_SHA256[:12]}; "
                           f"install {ADTOF_PYTORCH}")
    if list(FRAME_RNN_THRESHOLDS) != THRESHOLDS:
        raise RuntimeError(f"adtof_pytorch's thresholds {FRAME_RNN_THRESHOLDS} != {THRESHOLDS}")
    model = create_frame_rnn_model(calculate_n_bins())
    model = load_pytorch_weights(model, path, strict=True)
    model.eval()
    return model


def label_file(path: Path) -> dict[str, Any]:
    """The labels for one audio file, in the kick_weighted schema."""
    import librosa
    import numpy as np
    import torch
    from adtof_pytorch import LABELS_5, PeakPicker, load_audio_for_model

    y, _ = librosa.load(str(path), sr=RMS_SR, mono=True)
    rms = float(np.sqrt(np.mean(y**2))) if len(y) else 0.0
    base = {"source": GENERATOR["algorithm"], "rms": rms, "weights": GENERATOR["params"]["weights"],
            "generator": GENERATOR}
    if rms < MIN_RMS:
        return {**base, "onsets": [], "kick_count": 0, "snare_count": 0, "hihat_count": 0,
                "total": 0, "skipped": True,
                "skip_reason": f"silent audio (RMS={rms:.6f} < {MIN_RMS})"}

    with torch.no_grad():
        act = _model()(load_audio_for_model(str(path))).cpu().numpy()
    peaks = PeakPicker(thresholds=THRESHOLDS, fps=FPS).pick(act, labels=LABELS_5)[0]
    onsets = sorted(
        ({"time": round(float(t), 4), "weight": CLASSES[p][1], "type": CLASSES[p][0]}
         for p, times in peaks.items() if p in CLASSES for t in times),
        key=lambda o: (o["time"], o["type"]),
    )
    counts = {k: sum(1 for o in onsets if o["type"] == k) for k in ("kick", "snare", "hihat")}
    return {**base, "onsets": onsets, "kick_count": counts["kick"],
            "snare_count": counts["snare"], "hihat_count": counts["hihat"], "total": len(onsets)}
