#!/bin/bash
# Run parallel 4-device music test on all 18 tracks
# Usage: ./run-all-tracks.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MUSIC_DIR="$SCRIPT_DIR/../music/edm"
PORTS="/dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2 /dev/ttyACM3"

TRACKS=(
  trance-party
  techno-minimal-01
  trance-infected-vibes
  trance-goa-mantra
  techno-minimal-emotion
  techno-deep-ambience
  techno-machine-drum
  edm-trap-electro
  techno-dub-groove
  afrobeat-feelgood-groove
  amapiano-vibez
  breakbeat-background
  breakbeat-drive
  dnb-energetic-breakbeat
  dnb-liquid-jungle
  dubstep-edm-halftime
  garage-uk-2step
  reggaeton-fuego-lento
)

RESULTS_FILE="$SCRIPT_DIR/../test-results/v24-all-tracks-summary.json"
ALL_RESULTS="["

echo "=========================================="
echo "  v24 Full Track Library Validation"
echo "  $(date)"
echo "  ${#TRACKS[@]} tracks × 4 devices"
echo "=========================================="

for i in "${!TRACKS[@]}"; do
  track="${TRACKS[$i]}"
  num=$((i + 1))
  echo ""
  echo "[$num/${#TRACKS[@]}] $track"

  AUDIO="$MUSIC_DIR/$track.mp3"
  GT="$MUSIC_DIR/$track.beats.json"

  if [ ! -f "$AUDIO" ]; then
    echo "  SKIP: audio file not found"
    continue
  fi
  if [ ! -f "$GT" ]; then
    echo "  SKIP: ground truth not found"
    continue
  fi

  OUTPUT=$(node "$SCRIPT_DIR/parallel-music-test.mjs" "$AUDIO" "$GT" $PORTS 2>&1)
  echo "$OUTPUT" | grep -v "^JSON_RESULT:"

  # Extract JSON result
  JSON_LINE=$(echo "$OUTPUT" | grep "^JSON_RESULT:" | sed 's/^JSON_RESULT://')
  if [ -n "$JSON_LINE" ]; then
    if [ "$i" -gt 0 ]; then
      ALL_RESULTS="$ALL_RESULTS,"
    fi
    ALL_RESULTS="$ALL_RESULTS$JSON_LINE"
  fi

  # 3-second gap between tracks for device settling
  if [ "$num" -lt "${#TRACKS[@]}" ]; then
    sleep 3
  fi
done

ALL_RESULTS="$ALL_RESULTS]"

# Save combined results
echo "$ALL_RESULTS" | python3 -m json.tool > "$RESULTS_FILE" 2>/dev/null || echo "$ALL_RESULTS" > "$RESULTS_FILE"

echo ""
echo "=========================================="
echo "  Complete! Results: $RESULTS_FILE"
echo "=========================================="
