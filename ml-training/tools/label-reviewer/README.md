# Label Reviewer

Web UI for reviewing **and correcting** onset training labels against audio waveforms.

Onset corrections are the human ground truth — once edited, they are the final
truth and must flow through training and validation. `prepare_dataset.py` and
the blinky-server validation harness both apply the overlay automatically (see
"How edits flow into training and validation" below).

## Quick Start

```bash
cd ml-training && make reviewer
# Open http://localhost:8765
```

The two default corpora are registered automatically — switch between them in
the **Corpus** dropdown at the top of the page; no restart:

| Corpus | Audio | Auto onsets | Human edits |
|---|---|---|---|
| `training` | `/mnt/storage/blinky-ml-data/audio/combined` | `labels/onsets_consensus/` | `labels/onsets_human/` (devtop-local) |
| `edm` | `blinky-test-player/music/edm/` | same dir (`*.onsets_consensus.json`) | same dir (`*.onsets_human.json`, **commits to git**) |

Edits to the `edm` corpus ride with `git pull` to blinkyhost, where the
validation harness applies them on the next test run. Edits to `training` stay
on devtop and are picked up by the next `prepare_dataset.py` run.

## Adding more corpora

Pass one or more `--corpus` flags. Each is `name=audio_dir,onset_dir,human_edits_dir[,beat_dir,librosa_dir]`:

```bash
python tools/label-reviewer/server.py \
    --corpus my_corpus=/path/to/audio,/path/to/onsets,/path/to/edits
```

`--no-defaults` skips the built-in `training` + `edm` corpora; pass it with
`--corpus` to register a custom-only set.

## Viewer Tab

The main view shows a horizontally scrollable waveform with label markers
overlaid. Click a marker to select it; the edit panel appears below the legend.

### Marker colors

- **Red** — Beat label (consensus_v5). Read-only.
- **Green (downbeat marker)** — Downbeat. Read-only.
- **Cyan** — Auto onset (3+ systems agree, high confidence).
- **Yellow** — Auto onset (1–2 systems, low confidence).
- **Orange** — Human-edited auto onset (time and/or strength changed).
- **Bright green** — Human-created onset (did not exist in auto labels).
- **Grey** — Human-removed onset (auto onset flagged as wrong; kept in the
  data so the disagreement is measurable, but rendered greyed out and excluded
  from the merged label list).

A white outline marks the currently selected label.

### Editing onsets

| Action | How |
|--------|-----|
| **Select** a label | Click the marker, or click a row in the Onset Consensus list. |
| **Move** in time | Drag the marker, OR select it and edit the time field in the edit panel + Save. |
| **Change strength** | Select, edit the strength field, Save. |
| **Create** at a position | `Shift`+click anywhere on the waveform, OR press `+ Add Label` (or `A`) and click once. The Add Label button is one-shot — it disables itself after one create. |
| **Remove** | Select, click `Delete`, or press `Delete` / `Backspace`. The label is flagged removed (not destroyed) and rendered greyed. |
| **Undelete** | Select a removed label, click `Undelete`. |
| **Restore original** | For an auto label that was edited or removed, click `Restore original` to clear the human patch entirely. |

Edits save automatically (debounced ~400 ms). Switching tracks flushes pending
saves. The `beforeunload` handler best-effort flushes via `sendBeacon`.

Beats are intentionally **not** editable — beat tracking is out of scope for
the onset-detection task (see `CLAUDE.md`: onset detection ≠ beat detection).

### Playback

- **Spacebar** — Play/pause
- Click on the waveform — Seek (no shift key)
- Playback time shown top-right; waveform scrolls to follow at higher zoom levels.

### Navigation

- `←` / `→` — Previous / next track (auto-saves pending edits first)
- `1`–`4` — Zoom levels
- `R` — Mark track reviewed
- `P` — Flag track as problem
- `A` — Toggle Add Label one-shot
- `Esc` — Cancel Add Label / deselect label
- `Delete` / `Backspace` — Delete selected label

### Status line

Above the edit panel, a status line shows the current human-edit stats vs the
auto baseline, e.g.:

```
human edits: 4 edited · 2 removed · 7 created (vs 142 auto labels)
```

This is the human-vs-automated 5-system disagreement count for the track.

## Track Browser Tab

Browse all tracks in a paginated table; filter by review status. Click a row to
jump to that track in the viewer.

## Storage

Auto labels live unchanged in each corpus's `onset_dir`. Human corrections
live in the corpus's `human_edits_dir`, one file per track:

`{human_edits_dir}/{stem}.onsets_human.json`:

```json
{
  "stem": "<track_stem>",
  "source": "onsets_consensus",
  "source_count": 142,
  "edits": {
    "7":  { "time": 1.234, "edited": true },
    "12": { "removed": true },
    "18": { "strength": 0.9, "edited": true },
    "44": { "time": 6.001, "strength": 0.7, "edited": true, "removed": true }
  },
  "created": [
    { "time": 5.678, "strength": 1.0 }
  ]
}
```

- `edits` — keyed by **string index into the auto onsets list**. A patch may
  contain `time`, `strength`, `edited`, `removed`. Absent fields fall back to
  the auto value.
- `created` — labels with no auto counterpart. Each entry has `time` and
  `strength`. Order is arbitrary; the UI sorts the merged list at render time.
- `source_count` — the auto label count at the time of editing. If the auto
  source is regenerated and differs, indices may no longer line up — this
  field is the canary.

To compute the merged human-corrected labels for a track:

```python
def apply_human_edits(auto_onsets, edits_doc):
    merged = []
    for i, o in enumerate(auto_onsets):
        patch = edits_doc.get("edits", {}).get(str(i), {})
        if patch.get("removed"):
            continue
        merged.append({
            **o,
            **{k: v for k, v in patch.items() if k in ("time", "strength")},
            "human_edited": bool(patch.get("edited")),
        })
    for c in edits_doc.get("created", []):
        merged.append({**c, "human_created": True})
    merged.sort(key=lambda x: x["time"])
    return merged
```

## Options

```bash
python tools/label-reviewer/server.py --port 8765

# Add a corpus on top of the defaults
python tools/label-reviewer/server.py \
    --corpus my_corpus=/path/to/audio,/path/to/onsets,/path/to/edits

# Replace defaults entirely
python tools/label-reviewer/server.py --no-defaults \
    --corpus only_one=/p/audio,/p/onsets,/p/edits
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8765 | Server port |
| `--corpus` | (defaults below) | `name=audio_dir,onset_dir,human_edits_dir[,beat_dir,librosa_dir]`. Repeatable. |
| `--no-defaults` | off | Skip the built-in `training` + `edm` corpora |
| `--review-state` | `/mnt/storage/.../labels/review_state.json` | Per-corpus review state file |

The reviewer probes both `<stem>.onsets_consensus.json` and `<stem>.onsets.json`
when reading auto onsets — it works against either filename convention.

## API

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/api/corpora` | Registered corpora + track counts |
| `GET`  | `/api/tracks?corpus=&page=&per_page=&status=` | Paginated track list (per corpus) |
| `GET`  | `/api/labels?corpus=&stem=` | All labels for a track, including `human_edits` overlay |
| `POST` | `/api/edits` | Body: `{corpus, stem, source, source_count, edits, created}` — atomic full-document write |
| `POST` | `/api/review` | Body: `{corpus, stem, status, notes}` |
| `GET`  | `/audio/<corpus>/<stem>.<ext>` | Audio file with HTTP Range support |

## How edits flow into training and validation

**Validation (blinkyhost).** The blinky-server validation harness's
`track_discovery.discover_tracks()` looks for a sibling `<stem>.onsets_human.json`
next to each audio file. If present, `load_ground_truth()` merges it via
`apply_human_edits()` (in `blinky_server/testing/onset_label_merge.py`). The
merged onsets carry a `source` field (`'auto' | 'auto_edited' | 'human'`) so
scoring can later split metrics by provenance.

Curate the EDM corpus in the reviewer → `git add blinky-test-player/music/edm/*.onsets_human.json`
→ `git commit && git push` → on blinkyhost: `git pull && sudo systemctl restart
blinky-server`. The next test run uses the corrected ground truth.

**Training (devtop).** `prepare_dataset.py` reads
`labels.onset_human_dirs` from the config (default: `/mnt/storage/.../onsets_human/`
plus `blinky-test-player/music/edm/` as a fallback). For each track, it merges
the overlay before generating frame targets. Created onsets bypass the
`min_systems` filter (they're human-confirmed). Edit footprint is logged per
track as `HUMAN_EDITS_APPLIED <stem> edited=N removed=M created=K`.

**Drift safety.** `generate_onset_consensus.py` refuses to overwrite the
auto onset file for any track that has a human overlay, unless explicitly
forced with `--force-regenerate --allow-stale-edits`. The merge function
loud-fails on `source_count` mismatch (`HumanEditDriftError`) so a stale
overlay never silently corrupts training or scoring.

## Keyboard reference

| Key | Action |
|-----|--------|
| `Space` | Play / pause |
| `←` / `→` | Previous / next track |
| `R` | Mark track reviewed |
| `P` | Flag track as problem |
| `A` | Toggle Add Label one-shot |
| `Shift`+click waveform | Create label at click position |
| `Delete` / `Backspace` | Delete selected label |
| `Esc` | Cancel Add Label / deselect |
| `1`–`4` | Zoom levels |
