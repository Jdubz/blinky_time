#!/bin/bash
# Deploy a trained NN model to all blinkyhost devices.
#
# Usage:
#   ./scripts/deploy-model.sh outputs/v18-pcen/best_model.pt configs/conv1d_w16_onset_v18.yaml
#   ./scripts/deploy-model.sh outputs/v18-pcen/best_model.pt configs/conv1d_w16_onset_v18.yaml --dry-run
#
# Steps:
#   1. Export PyTorch model → TFLite INT8 → C header
#   2. Copy header into firmware source tree
#   3. Compile firmware (with version bump)
#   4. Copy hex to blinkyhost
#   5. Flash all serial devices via blinky-server API
#
# Requires: ml-training venv, arduino-cli, ssh access to blinkyhost

set -euo pipefail

MODEL_PATH="${1:?Usage: $0 <model.pt> <config.yaml> [--dry-run]}"
CONFIG_PATH="${2:?Usage: $0 <model.pt> <config.yaml> [--dry-run]}"
DRY_RUN="${3:-}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ML_DIR="$REPO_ROOT/ml-training"
FW_DIR="$REPO_ROOT/blinky-things"
BLINKYHOST="blinkyhost.local"
SERVER_URL="http://$BLINKYHOST:8420"

echo "=== Deploy Model ==="
echo "Model: $MODEL_PATH"
echo "Config: $CONFIG_PATH"

# 1. Export to TFLite + C header
echo ""
echo "--- Step 1: Export TFLite ---"
cd "$ML_DIR"
source venv/bin/activate

# Determine output dir from model path
OUTPUT_DIR="$(dirname "$MODEL_PATH")"
python scripts/export_tflite.py \
    --config "$CONFIG_PATH" \
    --model "$MODEL_PATH" \
    --output-dir "$OUTPUT_DIR"

# Find the generated header
HEADER_NAME=$(python -c "
from scripts.audio import load_config
cfg = load_config('$CONFIG_PATH')
print(cfg['export']['output_header'])
")
HEADER_PATH="$REPO_ROOT/$HEADER_NAME"

if [ ! -f "$HEADER_PATH" ]; then
    # Header might be relative to ml-training
    HEADER_PATH="$ML_DIR/$HEADER_NAME"
fi

if [ ! -f "$HEADER_PATH" ]; then
    echo "ERROR: Generated header not found at $HEADER_NAME"
    exit 1
fi

echo "Header: $HEADER_PATH"
HEADER_SIZE=$(wc -c < "$HEADER_PATH")
echo "Size: $HEADER_SIZE bytes"

# 2. Copy header to firmware source tree (as the active model)
echo ""
echo "--- Step 2: Copy header ---"
ACTIVE_HEADER="$FW_DIR/audio/frame_onset_model_data.h"
cp "$HEADER_PATH" "$ACTIVE_HEADER"
echo "Copied to $ACTIVE_HEADER"

# Check for PCEN define
if grep -q "ONSET_MODEL_USE_PCEN" "$ACTIVE_HEADER"; then
    echo "PCEN: enabled"
else
    echo "PCEN: disabled"
fi

# 3. Compile firmware
echo ""
echo "--- Step 3: Compile ---"
cd "$REPO_ROOT"
./scripts/build.sh

BUILD_NUM=$(cat "$FW_DIR/BUILD_NUMBER")
HEX_PATH="/tmp/blinky-build/blinky-things.ino.hex"

if [ ! -f "$HEX_PATH" ]; then
    echo "ERROR: Hex not found at $HEX_PATH"
    exit 1
fi
echo "Build: b$BUILD_NUM"

if [ "$DRY_RUN" = "--dry-run" ]; then
    echo ""
    echo "=== DRY RUN — stopping before flash ==="
    echo "Hex ready at: $HEX_PATH"
    exit 0
fi

# 4. Copy hex to blinkyhost
echo ""
echo "--- Step 4: Copy to blinkyhost ---"
scp "$HEX_PATH" "blinkytime@$BLINKYHOST:/tmp/blinky-things.ino.hex"
echo "Copied to $BLINKYHOST:/tmp/blinky-things.ino.hex"

# 5. Flash all serial devices
echo ""
echo "--- Step 5: Flash devices ---"
DEVICE_IDS=$(curl -s "$SERVER_URL/api/devices" | python3 -c "
import json, sys
devs = json.load(sys.stdin)
ids = [d['id'] for d in devs if d.get('transport') == 'serial' and d.get('state') == 'connected']
print(' '.join(ids))
")

if [ -z "$DEVICE_IDS" ]; then
    echo "ERROR: No serial devices connected"
    exit 1
fi

FLASH_COUNT=0
FLASH_FAIL=0
for dev_id in $DEVICE_IDS; do
    echo "Flashing ${dev_id:0:12}..."
    result=$(curl -s -X POST "$SERVER_URL/api/devices/$dev_id/flash" \
        -H 'Content-Type: application/json' \
        -d '{"firmware_path": "/tmp/blinky-things.ino.hex"}' 2>/dev/null)
    status=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','error'))" 2>/dev/null)
    if [ "$status" = "ok" ]; then
        echo "  OK"
        FLASH_COUNT=$((FLASH_COUNT + 1))
    else
        echo "  FAILED: $result"
        FLASH_FAIL=$((FLASH_FAIL + 1))
    fi
done

echo ""
echo "=== Deploy Complete ==="
echo "Build: b$BUILD_NUM"
echo "Flashed: $FLASH_COUNT devices ($FLASH_FAIL failed)"
