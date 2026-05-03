# Label Reviewer

Web UI for reviewing **and correcting** onset training labels against audio waveforms.

Onset corrections are the human ground truth — once edited, they are the final
truth and must flow through training and validation. The tool itself only edits
onsets; the next step is wiring corrections into `prepare_dataset.py` and the
GT validation harness so model training and Onset F1 both see the corrected
labels.

## Quick Start

```bash
cd ml-training
python tools/label-reviewer/server.py
# Open http://localhost:8765
```

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

Auto labels live unchanged in `--onset-labels`
(`/mnt/storage/blinky-ml-data/labels/onsets_consensus/`).

Human corrections live in a parallel directory, one file per track:

`{--human-edits}/{stem}.onsets_human.json`:

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
python tools/label-reviewer/server.py --audio-dir /path/to/audio
python tools/label-reviewer/server.py --onset-labels /path/to/onsets
python tools/label-reviewer/server.py --human-edits /path/to/edits
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8765 | Server port |
| `--audio-dir` | `/mnt/storage/blinky-ml-data/audio/combined` | Audio files |
| `--beat-labels` | `.../labels/consensus_v5` | Beat label directory (read-only) |
| `--onset-labels` | `.../labels/onsets_consensus` | Auto onset labels (read-only) |
| `--librosa-onsets` | `.../labels/onsets_librosa` | Librosa onset cache |
| `--human-edits` | `.../labels/onsets_human` | Where edit overlays are written |

## API

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/api/tracks?page=&per_page=&status=` | Paginated track list |
| `GET`  | `/api/labels?stem=` | All labels for a track, including `human_edits` overlay |
| `POST` | `/api/edits` | Body: full edit doc `{stem, source, source_count, edits, created}` — atomic full-document write |
| `POST` | `/api/review` | Body: `{stem, status, notes}` — review state |
| `GET`  | `/audio/<stem>.<ext>` | Audio file with HTTP Range support |

## Next step (not yet done)

Wire human edits into the training pipeline so corrections actually move
metrics. Specifically:

1. `ml-training/scripts/prepare_dataset.py` should load `--human-edits` and
   apply `apply_human_edits()` to every track that has an edit overlay before
   emitting training targets.
2. The blinky-server validation harness should apply the same merge to
   `.onsets_consensus.json` GT before scoring Onset F1, so a model that fires
   on a human-created onset gets credit and one that fires on a human-removed
   onset gets penalised.

Until both are done, the corrected data is collected but not leveraged — the
whole point of the system. Treat the tool as incomplete.

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
