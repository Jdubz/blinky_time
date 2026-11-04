"""Repetition consistency (#340): does repeated audio get repeated light?

Music repeats. Where a track's audio at time t matches its audio one repeat
later (t + L), a detector that follows the music as a person hears it fires
the same hits at both, with the same strengths. Where it doesn't, the same
hit shows in one bar and is silent in the next: "somewhat random which hits
trigger". This scores exactly that, and it needs no labels: the reference is
the track's own audio.

  1. Features: log-mel frames of the clean decoded file (not the device-level
     copy, whose quantization noise is part of what is being tested).
  2. Repeat lag L: the peak of the mean frame-to-frame similarity over lags in
     [LAG_MIN_S, LAG_MAX_S] that stands highest above the curve's local median
     (short lags are always more alike, so the raw maximum drifts to the floor).
     Only local maxima count, never an end of the range; refined to a fraction of a frame.
  3. Repeated regions: frames whose +-PATCH_S patch correlates with the patch
     one lag later above that track's own null: the same correlation at random
     lags off the 16th-note grid (L may span 1, 2 or 4 bars: the most that keeps
     a bar >= MIN_BAR_S), where the audio may genuinely repeat, at least
     NULL_OFF_GRID of a grid step from every grid line; quantile NULL_QUANTILE.
     Breakdowns, fills and transitions drop out, so only audio that really
     repeats is scored.
  4. Agreement: detections in a repeated region, matched one-to-one with
     detections one lag later (+-MATCH_TOL_S).

  rep       2 * matched / (detections in the regions + detections one lag later).
            1 = every hit that fires in one repeat fires in the next.
  repS      Strength agreement: 1 - sum|s_a - s_b| / sum max(s_a, s_b), where an
            unmatched detection pairs with 0. 1 = same hits at the same strengths.
  coverage  Share of the scored time that is a repeated region.
  n         Detections scored (in a repeated region, or one lag after one).
            Below MIN_SCORED, rep and repS are NaN: too few to say anything.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

SR = 16000
HOP = 256  # the firmware's frame: 16 ms, 62.5 fps
WIN = 512
N_MELS = 40
FMIN, FMAX = 30.0, 8000.0
LAG_MIN_S, LAG_MAX_S = 1.5, 8.0  # a bar at 160 BPM up to two bars at 60
PATCH_S = 0.25
NULL_QUANTILE = 0.99
NULL_LAGS = 64
MIN_BAR_S = 1.3  # a 4-beat bar at 185 BPM, faster than anything in edm/
NULL_OFF_GRID = 0.35  # of a 16th-note step: null lags sit between grid lines
BASELINE_S = 0.5  # local-median window of the lag curve
MATCH_TOL_S = 0.032  # two frames: a hit that moves by one frame still counts
MIN_SCORED = 20  # fewer detections in repeated audio than this is no evidence either way


@dataclass(frozen=True)
class Repetition:
    lag_s: float
    frame_s: float
    mask: np.ndarray  # per frame: True where the patch here repeats one lag later

    def repeated(self, t: np.ndarray) -> np.ndarray:
        i = np.round(np.asarray(t) / self.frame_s).astype(int)
        ok = (i >= 0) & (i < len(self.mask))
        out = np.zeros(len(i), dtype=bool)
        out[ok] = self.mask[i[ok]]
        return out


def _mel_filterbank() -> np.ndarray:
    def hz_to_mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    edges = 700.0 * (10 ** (np.linspace(hz_to_mel(FMIN), hz_to_mel(FMAX), N_MELS + 2) / 2595.0) - 1.0)
    bins = np.fft.rfftfreq(WIN, 1.0 / SR)
    fb = np.zeros((N_MELS, len(bins)))
    for m in range(N_MELS):
        lo, mid, hi = edges[m], edges[m + 1], edges[m + 2]
        fb[m] = np.clip(np.minimum((bins - lo) / (mid - lo), (hi - bins) / (hi - mid)), 0.0, None)
    assert (fb.sum(axis=1) > 0).all(), "empty mel band"
    return fb


def log_mel(x: np.ndarray) -> np.ndarray:
    """Log-mel frames (n_frames, N_MELS) at the firmware's hop, from float samples."""
    if len(x) < WIN:
        raise ValueError(f"audio too short for one frame ({len(x)} samples)")
    n = 1 + (len(x) - WIN) // HOP
    idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
    mag = np.abs(np.fft.rfft(x[idx] * np.hanning(WIN), axis=1))
    return np.log1p(1000.0 * (mag @ _mel_filterbank().T))


def _window_sums(v: np.ndarray, half: int) -> np.ndarray:
    """Sum over frames [i-half, i+half] (clipped at the ends), for each frame i."""
    c = np.concatenate([[0.0], np.cumsum(v)])
    i = np.arange(len(v))
    return c[np.minimum(i + half + 1, len(v))] - c[np.maximum(i - half, 0)]


def _patch_corr(f: np.ndarray, lag: int, half: int) -> np.ndarray:
    """Pearson r between the patch at frame i and the patch at i + lag, for i < n - lag."""
    a, b = f[:-lag], f[lag:]
    k = _window_sums(np.ones(len(a)), half) * f.shape[1]
    sa, sb = _window_sums(a.sum(1), half), _window_sums(b.sum(1), half)
    saa, sbb = _window_sums((a * a).sum(1), half), _window_sums((b * b).sum(1), half)
    sab = _window_sums((a * b).sum(1), half)
    cov = sab - sa * sb / k
    var = (saa - sa * sa / k) * (sbb - sb * sb / k)
    # A flat patch (digital silence) has no variance: it repeats nothing, so r = 0.
    return np.where(var > 1e-12, cov / np.sqrt(np.maximum(var, 1e-12)), 0.0)


def find_repetition(x: np.ndarray, seed: int = 0) -> Repetition:
    """The repeat lag and the repeated regions of clean float audio at 16 kHz."""
    f = log_mel(x)
    f = f - f.mean(axis=0)
    frame_s = HOP / SR
    lo, hi = int(LAG_MIN_S / frame_s), int(LAG_MAX_S / frame_s)
    if len(f) < 2 * hi:
        raise ValueError(f"audio too short to find a repeat ({len(f) * frame_s:.1f} s)")
    norm = f / np.maximum(np.linalg.norm(f, axis=1, keepdims=True), 1e-9)
    sim = np.array([(norm[:-lag] * norm[lag:]).sum(1).mean() for lag in range(lo, hi + 1)])
    peaks = np.flatnonzero((sim[1:-1] > sim[:-2]) & (sim[1:-1] >= sim[2:])) + 1
    if not len(peaks):
        raise ValueError("no repeat: frame similarity has no peak inside the lag range")
    w = int(round(BASELINE_S / frame_s))
    base = np.array([np.median(sim[max(0, i - w):i + w + 1]) for i in range(len(sim))])
    k = int(peaks[np.argmax((sim - base)[peaks])])
    y0, y1, y2 = sim[k - 1], sim[k], sim[k + 1]
    den = y0 - 2 * y1 + y2
    frac = 0.5 * (y0 - y2) / den if den < 0 else 0.0
    lag = lo + k
    half = int(round(PATCH_S / frame_s))
    r = _patch_corr(f, lag, half)
    rng = np.random.default_rng(seed)
    bars = 1
    while (lag + frac) * frame_s / (2 * bars) >= MIN_BAR_S:
        bars *= 2
    grid = (lag + frac) / (16 * bars)  # a 16th note, in frames
    cands = [L for L in range(lo, hi + 1) if abs(L / grid - round(L / grid)) > NULL_OFF_GRID]
    if not cands:
        raise ValueError("no off-grid lag for the null")
    null = np.concatenate([_patch_corr(f, int(L), half)[::half]
                           for L in rng.choice(cands, min(NULL_LAGS, len(cands)), replace=False)])
    thr = float(np.quantile(null, NULL_QUANTILE))
    mask = np.zeros(len(f), dtype=bool)
    mask[:len(r)] = r > thr
    return Repetition(lag_s=(lag + frac) * frame_s, frame_s=frame_s, mask=mask)


def _match(a: np.ndarray, b: np.ndarray, tol: float) -> list[tuple[int, int]]:
    """One-to-one pairs (i, j) with |a_i - b_j| <= tol, closest first."""
    cand = []
    for i, ai in enumerate(a):
        for j in range(np.searchsorted(b, ai - tol), np.searchsorted(b, ai + tol, "right")):
            cand.append((abs(ai - b[j]), i, j))
    cand.sort()
    used_a, used_b, pairs = set(), set(), []
    for d, i, j in cand:
        if d <= tol and i not in used_a and j not in used_b:
            used_a.add(i)
            used_b.add(j)
            pairs.append((i, j))
    return pairs


def consistency(times: np.ndarray, strengths: np.ndarray, rep: Repetition,
                t_start: float, t_end: float) -> dict:
    """rep / repS / coverage for detections (seconds, 0-1 strengths) scored over [t_start, t_end]."""
    times = np.asarray(times, dtype=float)
    strengths = np.asarray(strengths, dtype=float)
    order = np.argsort(times)
    times, strengths = times[order], strengths[order]
    L = rep.lag_s
    lo, hi = t_start, t_end - L
    frames = np.arange(int(np.ceil(lo / rep.frame_s)), int(hi / rep.frame_s) + 1)
    coverage = float(rep.repeated(frames * rep.frame_s).mean()) if len(frames) else float("nan")
    # A pair counts when either side lies in the region; a lone detection only
    # when it lies strictly inside. Otherwise a hit on a region's edge loses its
    # partner to a fraction of a frame of lag error (or the tolerance).
    tol = MATCH_TOL_S
    grow = int(np.ceil(tol / rep.frame_s))
    near = np.convolve(rep.mask.astype(int), np.ones(2 * grow + 1, dtype=int), "same") > 0
    near_rep = Repetition(lag_s=L, frame_s=rep.frame_s, mask=near)

    def side(t: np.ndarray):
        inside = (t >= lo) & (t <= hi) & rep.repeated(t)
        cand = (t >= lo - tol) & (t <= hi + tol) & near_rep.repeated(t)
        return cand, inside

    cand_a, inside_a = side(times)
    shifted = times - L
    cand_b, inside_b = side(shifted)
    ta, sa, ka = times[cand_a], strengths[cand_a], inside_a[cand_a]
    tb, sb, kb = shifted[cand_b], strengths[cand_b], inside_b[cand_b]
    pairs = [(i, j) for i, j in _match(ta, tb, tol) if ka[i] or kb[j]]
    ia = {i for i, _ in pairs}
    ib = {j for _, j in pairs}
    lone_a = [i for i in range(len(ta)) if ka[i] and i not in ia]
    lone_b = [j for j in range(len(tb)) if kb[j] and j not in ib]
    n = 2 * len(pairs) + len(lone_a) + len(lone_b)
    if n < MIN_SCORED:
        return {"rep": float("nan"), "rep_s": float("nan"), "coverage": coverage, "lag_s": L, "n": n}
    diff = sum(abs(sa[i] - sb[j]) for i, j in pairs)
    top = sum(max(sa[i], sb[j]) for i, j in pairs)
    lone = sa[lone_a].sum() + sb[lone_b].sum()
    den = top + lone
    return {
        "rep": 2 * len(pairs) / n,
        "rep_s": 1.0 - (diff + lone) / den if den > 0 else float("nan"),
        "coverage": coverage,
        "lag_s": L,
        "n": n,
    }
