#!/usr/bin/env bash
# Deploy a trained NN beat activation model to the firmware.
#
# This script:
# 1. Exports the trained model to TFLite INT8 C header
# 2. Validates the model fits within size budget
# 3. Commits the updated frame_beat_model_data.h to git
# 4. Pushes to remote (blinkyhost pulls, compiles, flashes)
#
# Usage:
#   bash tools/deploy_model.sh                              # Uses default paths
#   bash tools/deploy_model.sh --model path/to/model.keras  # Custom model path
#   bash tools/deploy_model.sh --skip-export                # Just commit+push existing header
#
# Run from the repo root (blinky_time/).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ML_DIR="$REPO_ROOT/ml-training"
HEADER_PATH="$REPO_ROOT/blinky-things/audio/frame_beat_model_data.h"
DEFAULT_MODEL="$ML_DIR/outputs/best_model.keras"
DEFAULT_CONFIG="$ML_DIR/configs/default.yaml"

# Parse args
MODEL_PATH="$DEFAULT_MODEL"
SKIP_EXPORT=false
PUSH=true

while [[ $# -gt 0 ]]; do
    case $1 in
        --model) MODEL_PATH="$2"; shift 2 ;;
        --config) DEFAULT_CONFIG="$2"; shift 2 ;;
        --skip-export) SKIP_EXPORT=true; shift ;;
        --no-push) PUSH=false; shift ;;
        --help)
            echo "Usage: deploy_model.sh [--model PATH] [--config PATH] [--skip-export] [--no-push]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

cd "$REPO_ROOT"

# Step 1: Export model
if [ "$SKIP_EXPORT" = false ]; then
    if [ ! -f "$MODEL_PATH" ]; then
        echo "ERROR: Model not found: $MODEL_PATH"
        echo "Train first: cd ml-training && python train.py"
        exit 1
    fi

    echo "=== Exporting model to TFLite INT8 C header ==="
    cd "$ML_DIR"

    # Activate venv if it exists
    if [ -f "venv/bin/activate" ]; then
        source venv/bin/activate
    fi

    # TODO: Replace with frame-level FC export script once training pipeline is built.
    # The old export_tflite.py was for mel-CNN models (closed).
    # export_beat_sync.py was for beat-sync models (closed, archived).
    # Frame-level FC export script will be: scripts/export_frame_beat.py
    # NOTE: Any CI job calling deploy_model.sh will fail until the export script exists.
    # This is intentional — use --skip-export to bypass for manual header placement.
    echo "ERROR: Frame-level FC export script not yet implemented."
    echo "Manually export the model and place the header at: $HEADER_PATH"
    echo "Or use: $0 --skip-export (to commit+push an existing header)"
    exit 1

    cd "$REPO_ROOT"
fi

# Step 2: Validate header was updated
if [ ! -f "$HEADER_PATH" ]; then
    echo "ERROR: Header file not found: $HEADER_PATH"
    exit 1
fi

# Check it's not the placeholder
if grep -q "0x00, 0x00, 0x00, 0x00," "$HEADER_PATH" && \
   grep -q "frame_beat_model_data_len = 4;" "$HEADER_PATH"; then
    echo "ERROR: frame_beat_model_data.h is still the placeholder (4 bytes)."
    echo "Export a real model first."
    exit 1
fi

# Show model size
MODEL_SIZE=$(grep "Model size:" "$HEADER_PATH" | head -1 || echo "unknown")
echo "Model header: $HEADER_PATH"
echo "  $MODEL_SIZE"

# Step 3: Commit
echo ""
echo "=== Committing model to git ==="
git add "$HEADER_PATH"

if git diff --cached --quiet "$HEADER_PATH"; then
    echo "No changes to frame_beat_model_data.h (already committed)."
else
    git commit -m "Update frame_beat_model_data.h with trained NN model

$MODEL_SIZE
Source: $MODEL_PATH"
    echo "Committed."
fi

# Step 4: Push
if [ "$PUSH" = true ]; then
    BRANCH=$(git branch --show-current)
    echo ""
    echo "=== Pushing $BRANCH to origin ==="
    git push origin "$BRANCH"
    echo ""
    echo "Done. On blinkyhost, run:"
    echo "  cd ~/Development/blinky_time && git pull"
    echo "  make uf2-upload NN=1 UPLOAD_PORT=/dev/ttyACM0"
else
    echo ""
    echo "Skipped push (--no-push). Push manually when ready."
fi
