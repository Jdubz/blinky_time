#!/bin/bash
# Pattern memory parameter sweep
#
# Sweeps confidenceRise (patrise), confidenceDecay (patfall), and
# histogramMinStrength (patminstren) across representative test tracks.
#
# Uses 3 tracks (~7 min per config): one 4otf (techno-minimal-01),
# one complex (dnb-energetic-breakbeat), one sparse (reggaeton-fuego-lento).
#
# Usage:
#   cd blinky-test-player
#   bash ../ml-training/tools/pattern_param_sweep.sh /dev/ttyACM1

set -e

PORT="${1:-/dev/ttyACM1}"
MUSIC_DIR="music/edm"
TRACKS="techno-minimal-01,dnb-energetic-breakbeat,reggaeton-fuego-lento"
RESULTS_DIR="tuning-results/pattern-sweep-$(date -u +%Y%m%d-%H%M%S)"
mkdir -p "$RESULTS_DIR"

# Helper: set a parameter via serial
set_param() {
    local port="$1" name="$2" value="$3"
    # Use node to send serial command
    NODE_PATH=node_modules node -e "
        const {SerialPort} = require('serialport');
        (async () => {
            const p = new SerialPort({path: '$port', baudRate: 115200});
            await new Promise(r => p.on('open', r));
            await new Promise(r => setTimeout(r, 300));
            p.write('set $name $value\n');
            await new Promise(r => setTimeout(r, 500));
            p.close();
        })().catch(e => { console.error(e.message); process.exit(1); });
    "
}

# Sweep configs: [patrise, patfall, patminstren]
# Baseline: 0.05, 0.15, 0.5
declare -a CONFIGS=(
    "0.05 0.15 0.5"   # baseline
    "0.10 0.15 0.5"   # faster rise
    "0.15 0.15 0.5"   # much faster rise
    "0.20 0.15 0.5"   # aggressive rise
    "0.10 0.10 0.5"   # faster rise + slower decay
    "0.10 0.05 0.5"   # faster rise + much slower decay
    "0.10 0.15 0.3"   # faster rise + lower threshold (more onsets)
    "0.10 0.15 0.6"   # faster rise + higher threshold (fewer onsets)
    "0.15 0.10 0.4"   # fast rise + slow decay + moderate threshold
)

echo "=== Pattern Memory Parameter Sweep ==="
echo "Port: $PORT"
echo "Tracks: $TRACKS"
echo "Configs: ${#CONFIGS[@]}"
echo "Results: $RESULTS_DIR"
echo ""

for i in "${!CONFIGS[@]}"; do
    read -r RISE FALL MINSTREN <<< "${CONFIGS[$i]}"
    CONFIG_NAME="rise${RISE}_fall${FALL}_min${MINSTREN}"
    echo "--- Config $((i+1))/${#CONFIGS[@]}: patrise=$RISE patfall=$FALL patminstren=$MINSTREN ---"

    # Set parameters
    set_param "$PORT" "patrise" "$RISE"
    set_param "$PORT" "patfall" "$FALL"
    set_param "$PORT" "patminstren" "$MINSTREN"
    sleep 1

    # Run test
    NODE_PATH=node_modules node ../ml-training/tools/pattern_memory_test.cjs \
        --ports "$PORT" --tracks "$TRACKS" --music-dir "$MUSIC_DIR" \
        2>&1 | tee "$RESULTS_DIR/${CONFIG_NAME}.log"

    # Copy results JSON
    LATEST=$(ls -t tuning-results/pattern-memory-*.json | head -1)
    if [ -n "$LATEST" ]; then
        cp "$LATEST" "$RESULTS_DIR/${CONFIG_NAME}.json"
    fi

    echo ""
done

# Reset to baseline
set_param "$PORT" "patrise" "0.05"
set_param "$PORT" "patfall" "0.15"
set_param "$PORT" "patminstren" "0.5"

echo "=== Sweep Complete ==="
echo "Results in: $RESULTS_DIR"

# Summary
echo ""
echo "=== Summary ==="
for f in "$RESULTS_DIR"/*.log; do
    name=$(basename "$f" .log)
    pass=$(grep "Tests passed:" "$f" | grep -oP '\d+/\d+' || echo "?/?")
    echo "  $name: $pass"
done
