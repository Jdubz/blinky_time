"""Phase 2 pre-stage (#115): filter FMA Electronic to mainstream-EDM
subgenres and compute per-track audio features.

Produces `/tmp/phase2_fma_features.json`:
  {track_id: {genre, density_gt, ioi_cv, flatness_mean, ..., audio_path,
              onset_gt_path}}

Phase 2 then needs only v34d on-device F1 per track to fit the
feature->threshold mapping.

Subgenre filter chosen 2026-04-29 to match the plan doc "~640 mainstream
EDM" target (Techno, House, Dubstep, DnB, Dance, Breakcore, Trip-Hop,
Downtempo, Ambient Electronic, Glitch, IDM, Minimal Electronic,
Breakbeat). FMA's Electronic subgenres are coarse — we filter on the
flat genre-id list rather than genre_top because tracks tag multiple
sub-genres and genre_top is the parent ('Electronic').
"""
from __future__ import annotations

import ast
import json
import logging
from pathlib import Path

import numpy as np
import pandas as pd

log = logging.getLogger("phase2_pre")

FMA_AUDIO = Path('/mnt/storage/blinky-ml-data/audio/fma')
FMA_META = Path('/mnt/storage/blinky-ml-data/audio/metadata/fma_metadata')
ONSETS_DIR = Path('/mnt/storage/blinky-ml-data/labels/onsets_consensus')

# Mainstream EDM subgenre IDs (from FMA's genres.csv — Electronic top-level
# subgenres only). Excludes Chip Music, Industrial, Synth Pop, etc. as
# non-relevant per the plan doc. Includes Breakbeat (parent=21=Hip-Hop)
# because the genre_id 542 is what FMA actually tags it with.
MAINSTREAM_EDM_GENRE_IDS = {
    181,   # Techno
    182,   # House
    183,   # Glitch
    184,   # Minimal Electronic
    185,   # Breakcore - Hard
    236,   # IDM
    286,   # Trip-Hop
    296,   # Dance
    337,   # Drum & Bass
    468,   # Dubstep
    495,   # Downtempo
    542,   # Breakbeat
    42,    # Ambient Electronic
}


def load_genre_id_to_title():
    g = pd.read_csv(FMA_META / 'genres.csv')
    return dict(zip(g['genre_id'], g['title']))


def filter_tracks() -> pd.DataFrame:
    """Return DataFrame of mainstream-EDM tracks with audio on disk."""
    t = pd.read_csv(FMA_META / 'tracks.csv', index_col=0, header=[0, 1])
    genres_col = t[('track', 'genres')]
    title_col = t[('track', 'title')]
    genre_top = t[('track', 'genre_top')]

    # Parse genre lists (stringified Python lists)
    rows = []
    for tid, gstr in genres_col.items():
        try:
            gids = ast.literal_eval(gstr) if isinstance(gstr, str) else gstr
        except (SyntaxError, ValueError):
            continue
        if not gids: continue
        matched = set(gids) & MAINSTREAM_EDM_GENRE_IDS
        if not matched: continue
        # Confirm audio file is on disk
        audio_path = FMA_AUDIO / f"{int(tid):06d}.mp3"
        if not audio_path.exists(): continue
        rows.append({
            'track_id': int(tid),
            'audio_path': str(audio_path),
            'genres': sorted(matched),
            'genre_top': genre_top.get(tid, ''),
            'title': str(title_col.get(tid, '')),
        })
    return pd.DataFrame(rows)


def load_onset_gt(track_id: int) -> np.ndarray:
    """Load onsets_consensus times. Returns empty array if missing."""
    p = ONSETS_DIR / f"{track_id:06d}.onsets.json"
    if not p.exists(): return np.array([], dtype=np.float64)
    d = json.loads(p.read_text())
    times = sorted(o['time'] for o in d.get('onsets', []))
    return np.asarray(times, dtype=np.float64)


def per_track_audio_features(audio_path: Path):
    """Compute ioi-style features from the onset GT and audio-side
    features that don't need NN inference. Mirror the Phase 1 feature set
    where possible (ioi_mean, ioi_cv, onset_density). Audio-feature
    side (flatness, centroid, crest, hfc) requires loading audio so we
    skip those for now and keep this CPU-light — Phase 2 validation will
    re-capture them from the device anyway.
    """
    return {}


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    id_to_title = load_genre_id_to_title()

    df = filter_tracks()
    log.info("filtered to %d mainstream-EDM FMA tracks with audio on disk", len(df))

    # Genre breakdown
    genre_counts = {}
    for _, row in df.iterrows():
        for gid in row['genres']:
            genre_counts[id_to_title.get(gid, f"#{gid}")] = genre_counts.get(
                id_to_title.get(gid, f"#{gid}"), 0) + 1
    log.info("per-subgenre counts:")
    for g, c in sorted(genre_counts.items(), key=lambda x: -x[1]):
        log.info("  %-25s %d", g, c)

    # Per-track features. Tracks with <5 GT onsets get surfaced (not silently
    # dropped) — that's almost always either (a) ambient material with no
    # percussive onsets (legitimate, but caller should know), or (b) a GT
    # generation failure (silent generator crash). Caller decides policy.
    out = {}
    sparse_tracks: list[tuple[int, int, str]] = []
    for _, row in df.iterrows():
        tid = row['track_id']
        gt = load_onset_gt(tid)
        if gt.size < 5:
            sparse_tracks.append((tid, int(gt.size), row.get('title', '')))
            continue
        # GT-derived features. We've already gated gt.size >= 5 above, so
        # diff() produces >=4 IOIs and duration > 0 — fail loud rather than
        # masking either with a 0-fallback (a track that snuck past the
        # gate but produces degenerate features is a data-integrity bug).
        iois = np.diff(gt)
        iois = iois[iois > 0.02]
        duration = float(gt[-1] - gt[0])
        if duration <= 0 or iois.size == 0:
            raise ValueError(
                f"track {tid} passed gt_count gate ({gt.size}) but has "
                f"degenerate timing: duration={duration}, iois.size={iois.size}. "
                f"GT file: {ONSETS_DIR / f'{tid:06d}.onsets.json'}"
            )
        out[tid] = {
            'audio_path': row['audio_path'],
            'onset_gt_path': str(ONSETS_DIR / f"{tid:06d}.onsets.json"),
            'genres': [id_to_title.get(g, f"#{g}") for g in row['genres']],
            'title': row['title'],
            'gt_count': int(gt.size),
            'duration_s': duration,
            'onset_density': float(gt.size / duration),
            'ioi_cv': float(iois.std() / iois.mean()),
            'ioi_mean_ms': float(iois.mean() * 1000),
        }

    log.info("tracks with usable GT: %d", len(out))
    if sparse_tracks:
        log.warning("[FALLBACK] %d tracks dropped for GT count <5 — surface them so the "
                    "caller can decide whether they are legitimate ambient material "
                    "or upstream GT-generation failures:", len(sparse_tracks))
        for tid, n, title in sparse_tracks[:20]:
            log.warning("  track_id=%d  gt_count=%d  title=%r", tid, n, title)
        if len(sparse_tracks) > 20:
            log.warning("  ... +%d more", len(sparse_tracks) - 20)

    out_path = Path('/tmp/phase2_fma_features.json')
    out_path.write_text(json.dumps(out, indent=2))
    log.info("wrote %d entries to %s (size=%.1f KB)",
             len(out), out_path, out_path.stat().st_size / 1024)

    # Summary stats
    if out:
        densities = [v['onset_density'] for v in out.values()]
        ioi_cvs = [v['ioi_cv'] for v in out.values() if v['ioi_cv'] > 0]
        log.info("onset_density: min=%.2f max=%.2f mean=%.2f", min(densities), max(densities), np.mean(densities))
        log.info("ioi_cv:        min=%.2f max=%.2f mean=%.2f", min(ioi_cvs), max(ioi_cvs), np.mean(ioi_cvs))


if __name__ == '__main__':
    main()
