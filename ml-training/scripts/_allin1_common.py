"""Shared utilities for allin1 helper scripts.

Used by both _allin1_helper.py (per-track subprocess) and
_allin1_batch_helper.py (long-lived batch subprocess).
"""

_dbn_threshold_patched = False


def patch_dbn_threshold():
    """Disable DBN threshold clipping for short clips.

    allin1's postprocessing passes best_threshold_downbeat (0.24) to madmom's
    DBNDownBeatTrackingProcessor, which crops the activation sequence to only
    frames exceeding the threshold before Viterbi decoding. After the 3-way
    normalization (beat/downbeat/no-beat), peak activations on 30s clips are
    compressed below this threshold, causing near-total beat loss. Setting
    threshold=None lets the HMM decode the full sequence — its tempo model
    and transition probabilities already handle noise.

    Must patch allin1.helpers (where the bound name lives), not just
    allin1.postprocessing.metrical (the defining module).

    Idempotent — safe to call multiple times.
    """
    global _dbn_threshold_patched
    if _dbn_threshold_patched:
        return
    _dbn_threshold_patched = True

    import allin1.helpers as helpers
    import allin1.postprocessing.metrical as metrical
    from allin1.typings import AllInOneOutput
    from allin1.config import Config

    _original = metrical.postprocess_metrical_structure

    def _patched(logits: AllInOneOutput, cfg: Config):
        orig_threshold = cfg.best_threshold_downbeat
        cfg.best_threshold_downbeat = None
        try:
            return _original(logits, cfg)
        finally:
            cfg.best_threshold_downbeat = orig_threshold

    metrical.postprocess_metrical_structure = _patched
    helpers.postprocess_metrical_structure = _patched
