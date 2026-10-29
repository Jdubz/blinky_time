#!/bin/bash
# Generate kick/snare labels from Demucs drum stems.
# Run AFTER batch_demucs_separate.py completes.
#
# Usage:
#   ./scripts/prepare_drum_labels.sh
#
# Steps:
#   1. Create flat directory of drum stem symlinks (generator expects flat dir)
#   2. Run generate_kick_weighted_targets.py on drum stems
#   3. Report results

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Storage roots as the configs name them (#286); unset, the old devtop layout.
DATA="${BLINKY_DATA:-/mnt/storage/blinky-ml-data}"
STEMS_DIR="${STEMS_DIR:-$DATA/stems/htdemucs}"
DRUMS_DIR="${DRUMS_DIR:-$DATA/audio/drums_only}"
# Named for the generator version (GENERATOR in generate_kick_weighted_targets.py): the
# generator refuses a dir another version wrote, so a new version needs a new dir (#284).
LABELS_DIR="${LABELS_DIR:-$DATA/labels/kick_weighted_drums_g1}"
# Which tracks to label: those with full-mix onset consensus (the clean step needs it).
TRACKS_FROM="${TRACKS_FROM:-$DATA/labels/onsets_consensus}"

echo "=== Drum-stem label generation ==="

# Step 1: Flat directory of drum stems
# generate_kick_weighted_targets.py expects {audio_dir}/{track}.wav and uses
# the filename stem to match consensus labels. The demucs output is nested:
# stems/htdemucs/{track}/drums.wav. We create a flat dir with named links.
echo "Linking drum stems..."
mkdir -p "$DRUMS_DIR"
COUNT=0
for d in "$STEMS_DIR"/*/drums.wav; do
    if [ -f "$d" ]; then
        TRACK=$(basename "$(dirname "$d")")
        ln -sf "$d" "$DRUMS_DIR/${TRACK}.wav"
        COUNT=$((COUNT + 1))
    fi
done
echo "  $COUNT drum stems linked in $DRUMS_DIR"

# Step 2: Generate kick-weighted onset labels on drum stems
echo "Generating kick/snare labels on drum stems..."
mkdir -p "$LABELS_DIR"
python "$SCRIPT_DIR/generate_kick_weighted_targets.py" \
    --audio-dir "$DRUMS_DIR" \
    --output-dir "$LABELS_DIR" \
    --tracks-from "$TRACKS_FROM" \
    --workers 4

# Step 3: Report
LABEL_COUNT=$(ls "$LABELS_DIR"/*.kick_weighted.json 2>/dev/null | wc -l)
echo ""
echo "=== Done ==="
echo "  Drum stems: $COUNT"
echo "  Labels generated: $LABEL_COUNT"
echo "  Output: $LABELS_DIR"
echo ""
echo "Next: clean them (scripts/clean_kick_weighted.py --kw-dir $LABELS_DIR) and validate"
echo "(scripts/validate_kick_weighted.py). Score any generator change with drum_label_suite.py."
