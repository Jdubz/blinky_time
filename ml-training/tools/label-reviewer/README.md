# Label Reviewer

Web UI for reviewing onset and beat training labels against audio waveforms.

## Quick Start

```bash
cd ml-training
python tools/label-reviewer/server.py
# Open http://localhost:8765
```

## Usage

### Viewer Tab

The main view shows a waveform with label markers overlaid:

- **Red lines** — Beat labels (consensus_v5, 7 beat-tracking systems). Intensity scales with consensus strength (2/7 = faint, 7/7 = solid).
- **Green lines** — Downbeat labels.
- **Cyan lines** — Onset consensus labels (5 onset detection systems, high confidence: 3+ systems agree).
- **Yellow lines** — Onset consensus labels (low confidence: 1-2 systems).

Click anywhere on the waveform to seek. Click a label in the side lists to jump to that time.

### Playback

- **Spacebar** — Play/pause
- Playback position shown in real-time (top right)
- Waveform scrolls to follow playback at higher zoom levels

### Navigation

- **Left/Right arrows** — Previous/next track
- Track name and position shown in the controls bar

### Zoom

- **1** — Overview (full track)
- **2** — Normal (default)
- **3** — Detailed
- **4** — Close-up (individual onsets visible)

Or use the dropdown in the controls bar.

### Label Filtering

Use the dropdown to show:
- **All labels** — Beats + onsets overlaid
- **Beats only** — Just beat labels
- **Onsets only** — Just onset consensus labels
- **No labels** — Clean waveform

### Review Workflow

For each track:
1. Listen and visually check labels against the waveform
2. **R** — Mark as reviewed (labels look correct)
3. **P** — Flag as problem (missing onsets, wrong timing, noise)
4. Add notes in the text field (persisted automatically)

Review state is saved to `/mnt/storage/blinky-ml-data/labels/review_state.json`.

### Track Browser Tab

Browse all tracks in a paginated table:
- Filter by status: all / unreviewed / reviewed / problem
- Click a row to jump to that track in the viewer

## Options

```bash
python tools/label-reviewer/server.py --port 8765
python tools/label-reviewer/server.py --audio-dir /path/to/audio
python tools/label-reviewer/server.py --onset-labels /path/to/onsets
```

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | 8765 | Server port |
| `--audio-dir` | `/mnt/storage/blinky-ml-data/audio/combined` | Audio files |
| `--beat-labels` | `.../labels/consensus_v5` | Beat label directory |
| `--onset-labels` | `.../labels/onsets_consensus` | Onset label directory |
| `--librosa-onsets` | `.../labels/onsets_librosa` | Librosa onset cache |

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / pause |
| `←` / `→` | Previous / next track |
| `R` | Mark reviewed |
| `P` | Flag problem |
| `1`-`4` | Zoom levels |
