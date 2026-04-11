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

STEMS_DIR="${STEMS_DIR:-/mnt/storage/blinky-ml-data/stems/htdemucs}"
DRUMS_DIR="${DRUMS_DIR:-/mnt/storage/blinky-ml-data/audio/drums_only}"
LABELS_DIR="${LABELS_DIR:-/mnt/storage/blinky-ml-data/labels/kick_weighted_drums}"

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
    --labels-dir /mnt/storage/blinky-ml-data/labels/consensus_v5 \
    --workers 4

# Step 3: Report
LABEL_COUNT=$(ls "$LABELS_DIR"/*.kick_weighted.json 2>/dev/null | wc -l)
echo ""
echo "=== Done ==="
echo "  Drum stems: $COUNT"
echo "  Labels generated: $LABEL_COUNT"
echo "  Output: $LABELS_DIR"
echo ""
echo "To train v20:"
echo "  ./train_pipeline.sh configs/conv1d_w16_onset_v20.yaml v20-drum-labels --skip-labels"
