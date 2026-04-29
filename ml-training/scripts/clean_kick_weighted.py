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
    """Return cleaned kw dict, or None to skip (missing/skipped/empty)."""
    if not cons_path.exists():
        return None
    kw = json.loads(kw_path.read_text())
    if kw.get("skipped"):
        return None
    cons = json.loads(cons_path.read_text())

    cons_supported = sorted(
        o["time"] for o in cons.get("onsets", [])
        if o.get("systems", 1) >= min_systems
    )
    cons_systems_by_time = {o["time"]: o.get("systems", 1) for o in cons.get("onsets", [])}

    if not cons_supported:
        kw["onsets"] = []
        kw["filter"] = {
            "method": "stem_vs_mix_consensus",
            "tolerance_s": tol_s,
            "min_systems": min_systems,
            "kept": 0,
            "dropped": len(kw.get("onsets", [])),
        }
        return kw

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
