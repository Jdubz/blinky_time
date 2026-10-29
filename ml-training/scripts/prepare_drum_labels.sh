#!/bin/bash
# Generate kick/snare labels (#284).
#
# Usage:
#   ./scripts/prepare_drum_labels.sh                 # ADTOF on the full mixes (default)
#   ALGORITHM=bandpass ./scripts/prepare_drum_labels.sh   # the old band generator, on stems
#
# ADTOF (scripts/adtof_labels.py) transcribes the full mix, so it needs no stems. The band
# generator needs Demucs drum stems: run batch_demucs_separate.py first.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Storage roots as the configs name them (#286); unset, the old devtop layout.
DATA="${BLINKY_DATA:-/mnt/storage/blinky-ml-data}"
ALGORITHM="${ALGORITHM:-adtof}"
# Which tracks to label: those with full-mix onset consensus (the clean step needs it).
TRACKS_FROM="${TRACKS_FROM:-$DATA/labels/onsets_consensus}"

echo "=== Drum label generation ($ALGORITHM) ==="

if [ "$ALGORITHM" = adtof ]; then
    AUDIO_DIR="${AUDIO_DIR:-$DATA/audio/combined}"
    # Named for the generator: it refuses a dir another generator wrote (#284).
    LABELS_DIR="${LABELS_DIR:-$DATA/labels/kick_weighted_adtof_v1}"
else
    STEMS_DIR="${STEMS_DIR:-$DATA/stems/htdemucs}"
    AUDIO_DIR="${DRUMS_DIR:-$DATA/audio/drums_only}"
    LABELS_DIR="${LABELS_DIR:-$DATA/labels/kick_weighted_drums_g1}"
    # The generator expects a flat {audio_dir}/{track}.wav; demucs writes
    # stems/htdemucs/{track}/drums.wav, so link them flat.
    echo "Linking drum stems..."
    mkdir -p "$AUDIO_DIR"
    COUNT=0
    for d in "$STEMS_DIR"/*/drums.wav; do
        if [ -f "$d" ]; then
            TRACK=$(basename "$(dirname "$d")")
            ln -sf "$d" "$AUDIO_DIR/${TRACK}.wav"
            COUNT=$((COUNT + 1))
        fi
    done
    echo "  $COUNT drum stems linked in $AUDIO_DIR"
fi

mkdir -p "$LABELS_DIR"
python "$SCRIPT_DIR/generate_kick_weighted_targets.py" \
    --algorithm "$ALGORITHM" \
    --audio-dir "$AUDIO_DIR" \
    --output-dir "$LABELS_DIR" \
    --tracks-from "$TRACKS_FROM" \
    --workers 4

LABEL_COUNT=$(find "$LABELS_DIR" -maxdepth 1 -name '*.kick_weighted.json' | wc -l)
echo ""
echo "=== Done ==="
echo "  Labels: $LABEL_COUNT in $LABELS_DIR"
echo ""
echo "Next: validate (scripts/validate_kick_weighted.py --label-dir $LABELS_DIR"
echo "--consensus-dir $TRACKS_FROM). Score any generator change with drum_label_suite.py."
