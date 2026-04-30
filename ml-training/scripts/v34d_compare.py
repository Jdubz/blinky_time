"""Compare two evaluate.py outputs side-by-side, joined by track name.

Both inputs are the JSON list emitted by evaluate.py at
`<output_dir>/eval/eval_results.json`. The primary metric is `f1`
(onset F1 against `.onsets_consensus.json` GT, mir_eval window=50ms),
which is now the apples-to-apples target for both v33 and v34d after
the 2026-04-29 evaluator fix.

Refuses to run if either side is empty or the track sets don't match —
silently scoring against a subset would let regressions hide.
"""
from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path


def load(path: Path) -> dict[str, dict]:
    if not path.exists():
        raise FileNotFoundError(f"eval results missing: {path}")
    data = json.loads(path.read_text())
    if not isinstance(data, list):
        raise ValueError(f"{path}: expected a list of per-track dicts, got {type(data).__name__}")
    if not data:
        raise ValueError(f"{path}: eval_results.json is empty — eval ran but produced zero results")
    return {r["track"]: r for r in data}


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: v34d_compare.py <baseline_eval_results.json> <candidate_eval_results.json>",
              file=sys.stderr)
        return 2
    base_path = Path(sys.argv[1])
    cand_path = Path(sys.argv[2])
    base = load(base_path)
    cand = load(cand_path)

    base_tracks = set(base)
    cand_tracks = set(cand)
    if base_tracks != cand_tracks:
        only_base = sorted(base_tracks - cand_tracks)
        only_cand = sorted(cand_tracks - base_tracks)
        raise ValueError(
            f"track sets differ — refusing to compare on a subset.\n"
            f"  only in baseline ({base_path.name}): {only_base[:10]}{' ...' if len(only_base) > 10 else ''}\n"
            f"  only in candidate ({cand_path.name}): {only_cand[:10]}{' ...' if len(only_cand) > 10 else ''}"
        )

    base_label = base_path.parent.parent.name  # e.g. v33_mel_only
    cand_label = cand_path.parent.parent.name  # e.g. v34d
    print(f"Comparison: baseline={base_label}  candidate={cand_label}")
    print(f"Tracks: {len(base_tracks)}\n")

    rows = []
    for t in sorted(base_tracks):
        b = base[t]
        c = cand[t]
        rows.append({
            "track": t,
            "f1_base": b.get("f1", float("nan")),
            "f1_cand": c.get("f1", float("nan")),
            "delta": c.get("f1", float("nan")) - b.get("f1", float("nan")),
            "ref": b.get("ref_beats", 0),
            "est_base": b.get("est_beats", 0),
            "est_cand": c.get("est_beats", 0),
        })

    print(f"{'track':<32} {'ref':>5} {'F1_base':>8} {'F1_cand':>8} {'Δ':>7} {'est_b':>6} {'est_c':>6}")
    print("-" * 80)
    for r in sorted(rows, key=lambda r: r["delta"]):
        sign = "↓" if r["delta"] < -0.01 else ("↑" if r["delta"] > 0.01 else " ")
        print(f"{r['track'][:32]:<32} {r['ref']:>5} {r['f1_base']:>8.3f} "
              f"{r['f1_cand']:>8.3f} {r['delta']:>+7.3f} {sign} "
              f"{r['est_base']:>5}  {r['est_cand']:>5}")

    f1_base = [r["f1_base"] for r in rows]
    f1_cand = [r["f1_cand"] for r in rows]
    deltas = [r["delta"] for r in rows]
    print()
    print(f"AGGREGATE (n={len(rows)}):")
    print(f"  baseline  ({base_label:<24}) F1 mean={statistics.mean(f1_base):.3f} median={statistics.median(f1_base):.3f}")
    print(f"  candidate ({cand_label:<24}) F1 mean={statistics.mean(f1_cand):.3f} median={statistics.median(f1_cand):.3f}")
    print(f"  delta mean = {statistics.mean(deltas):+.3f}, median = {statistics.median(deltas):+.3f}")
    n_up = sum(1 for d in deltas if d > 0.01)
    n_down = sum(1 for d in deltas if d < -0.01)
    n_flat = len(deltas) - n_up - n_down
    print(f"  per-track: {n_up} up, {n_flat} flat (Δ<0.01), {n_down} down")

    # Verdict
    print()
    delta_mean = statistics.mean(deltas)
    if delta_mean > 0.02:
        print(f"VERDICT: candidate beats baseline by {delta_mean:+.3f} on average — promote.")
    elif delta_mean < -0.02:
        print(f"VERDICT: candidate REGRESSES baseline by {delta_mean:+.3f} — investigate.")
    else:
        print(f"VERDICT: mean delta {delta_mean:+.3f} within noise — see per-track breakdown.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
