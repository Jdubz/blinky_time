"""Clean kick_weighted labels via stem-vs-mix consensus filter.

Removes drum-stem bleed by requiring each kick_weighted onset to be
corroborated by an onsets_consensus event with >=2 systems within +-70ms.

Validated on edm/ vs hand-curated GT (audit 2026-04-29):
  baseline kw      -> F1=0.657, P=0.65, R=0.72
  kw cap cons>=2 (+-70ms) -> F1=0.745, P=0.91, R=0.65  (chosen)

Higher precision favours training; we drop ~35% of labels as likely bleed.

Inputs:
  --kw-dir         labels/kick_weighted_drums (per-track *.kick_weighted.json)
  --consensus-dir  labels/onsets_consensus    (per-track *.onsets.json)
  --output-dir     labels/kick_weighted_clean (created if missing)

Output schema mirrors kw input but each onset has additional fields:
  consensus_systems  int  # systems backing the matched consensus event
  consensus_dt_ms    float # |kw_time - matched_consensus_time| * 1000
"""
from __future__ import annotations

import argparse
import json
import logging
from pathlib import Path

import numpy as np

log = logging.getLogger("clean_kw")


def filter_track(kw_path: Path, cons_path: Path, tol_s: float, min_systems: int) -> dict | None:
    """Return cleaned kw dict, or None ONLY for the legitimate skip case
    (kw.skipped=true means upstream drum separation failed for this track —
    that's a data fact, not a script-level decision).

    All other failure modes raise loudly: missing consensus, malformed JSON,
    inconsistent metadata. Silently swallowing those would let the cleaned
    label corpus differ from what we think it contains, which would poison
    downstream training.
    """
    kw = json.loads(kw_path.read_text())
    if kw.get("skipped"):
        # Legitimate: drum-stem separation failed upstream for this track.
        # The kw labeler explicitly marks the track as unusable; honoring
        # that flag is not a fallback.
        return None
    if not cons_path.exists():
        raise FileNotFoundError(
            f"kw labels exist but consensus is missing for {kw_path.stem}: "
            f"expected {cons_path}. Both label sources should be generated "
            f"from the same audio corpus; a gap means the multi-system "
            f"labeling pipeline didn't finish for this track. Re-run "
            f"label_beats.py + merge_consensus_labels_v2.py before retrying."
        )
    cons = json.loads(cons_path.read_text())

    cons_supported = sorted(
        o["time"] for o in cons.get("onsets", [])
        if o.get("systems", 1) >= min_systems
    )
    # Float identity assumption: cons_supported is built from o["time"] and
    # then queried below via float(cons_arr[idx]). Both sides are the same
    # float bit pattern (sorted() and np.asarray() preserve floats exactly,
    # and float(np.float64) round-trips losslessly). The dict lookup at
    # line ~95 only works because of this — DO NOT introduce arithmetic on
    # nearest_t before the lookup or the keys will mismatch.
    cons_systems_by_time = {o["time"]: o.get("systems", 1) for o in cons.get("onsets", [])}

    if not cons_supported:
        # Empty consensus AT min_systems threshold means either (a) the
        # track has very few onsets even by lenient detectors (likely
        # ambient/silent — surface it), or (b) min_systems is set too
        # high. Either way, dropping ALL kw onsets silently is wrong —
        # the user needs to see this case to decide policy.
        raise ValueError(
            f"No consensus onsets at min_systems>={min_systems} for "
            f"{kw_path.stem}: consensus file has "
            f"{len(cons.get('onsets', []))} total onsets, but none are "
            f"backed by >={min_systems} systems. Lower --min-systems or "
            f"investigate the track."
        )

    cons_arr = np.asarray(cons_supported)
    kept = []
    dropped = 0
    for o in kw.get("onsets", []):
        t = o["time"]
        idx = int(np.searchsorted(cons_arr, t))
        nearest_t = None
        nearest_dt = float("inf")
        for j in (idx - 1, idx):
            if 0 <= j < len(cons_arr):
                dt = abs(cons_arr[j] - t)
                if dt < nearest_dt:
                    nearest_dt = dt
                    nearest_t = float(cons_arr[j])
        if nearest_t is not None and nearest_dt <= tol_s:
            o = dict(o)
            o["consensus_systems"] = cons_systems_by_time.get(nearest_t, min_systems)
            o["consensus_dt_ms"] = round(nearest_dt * 1000.0, 2)
            kept.append(o)
        else:
            dropped += 1

    kw["onsets"] = kept
    kw["filter"] = {
        "method": "stem_vs_mix_consensus",
        "tolerance_s": tol_s,
        "min_systems": min_systems,
        "kept": len(kept),
        "dropped": dropped,
    }
    return kw


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--kw-dir", type=Path,
                    default=Path("/mnt/storage/blinky-ml-data/labels/kick_weighted_drums"))
    ap.add_argument("--consensus-dir", type=Path,
                    default=Path("/mnt/storage/blinky-ml-data/labels/onsets_consensus"))
    ap.add_argument("--output-dir", type=Path,
                    default=Path("/mnt/storage/blinky-ml-data/labels/kick_weighted_clean"))
    ap.add_argument("--tolerance-ms", type=float, default=70.0)
    ap.add_argument("--min-systems", type=int, default=2)
    ap.add_argument("--limit", type=int, default=0,
                    help="Process at most N tracks (0 = all). Useful for dry runs.")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    tol_s = args.tolerance_ms / 1000.0

    kw_files = sorted(args.kw_dir.glob("*.kick_weighted.json"))
    if args.limit:
        kw_files = kw_files[:args.limit]
    log.info("found %d kw files", len(kw_files))

    n_done = n_skipped = 0
    total_kept = total_dropped = 0
    for i, kw_path in enumerate(kw_files):
        track = kw_path.name.removesuffix(".kick_weighted.json")
        cons_path = args.consensus_dir / f"{track}.onsets.json"
        out_path = args.output_dir / kw_path.name

        cleaned = filter_track(kw_path, cons_path, tol_s, args.min_systems)
        if cleaned is None:
            n_skipped += 1
            continue
        out_path.write_text(json.dumps(cleaned))
        n_done += 1
        total_kept += cleaned["filter"]["kept"]
        total_dropped += cleaned["filter"]["dropped"]

        if (i + 1) % 500 == 0:
            log.info("progress: %d/%d done=%d skipped=%d kept=%d dropped=%d",
                     i + 1, len(kw_files), n_done, n_skipped, total_kept, total_dropped)

    log.info("DONE done=%d skipped=%d kept=%d dropped=%d (kept frac=%.1f%%)",
             n_done, n_skipped, total_kept, total_dropped,
             100.0 * total_kept / max(1, total_kept + total_dropped))


if __name__ == "__main__":
    main()
