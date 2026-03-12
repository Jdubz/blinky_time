#!/bin/bash
# Post-training evaluation and export for frame-level models.
#
# Usage:
#   ./eval_and_export.sh frame_fc_full     # Evaluate + export frame FC
#   ./eval_and_export.sh frame_conv1d_full # Evaluate + export frame Conv1D
#
# Runs: validation set eval → on-track eval (EDM test set) → TFLite INT8 export
# Requires: venv activated, GPU available

set -e

RUN_NAME="${1:?Usage: $0 <run_name> (e.g. frame_fc_full or frame_conv1d_full)}"
OUTPUT_DIR="outputs/${RUN_NAME}"
EVAL_DIR="${OUTPUT_DIR}/eval"

# Determine config from run name
if [[ "$RUN_NAME" == *"conv1d_wide"* ]]; then
    CONFIG="configs/frame_conv1d_wide.yaml"
elif [[ "$RUN_NAME" == *"conv1d"* ]]; then
    CONFIG="configs/frame_conv1d.yaml"
elif [[ "$RUN_NAME" == *"fc"* ]]; then
    CONFIG="configs/frame_fc.yaml"
else
    echo "Cannot determine config for run: $RUN_NAME"
    exit 1
fi

MODEL="${OUTPUT_DIR}/best_model.pt"

if [ ! -f "$MODEL" ]; then
    echo "ERROR: Model not found at $MODEL"
    exit 1
fi

echo "============================================"
echo "Post-Training Eval & Export: ${RUN_NAME}"
echo "  Config: ${CONFIG}"
echo "  Model:  ${MODEL}"
echo "============================================"

# 1. Validation set evaluation
echo ""
echo "=== Validation Set Evaluation ==="
python evaluate.py --config "$CONFIG" --model "$MODEL" --output-dir "$EVAL_DIR"

# 2. On-track evaluation (EDM test set)
echo ""
echo "=== On-Track Evaluation (EDM) ==="
python evaluate.py --config "$CONFIG" --model "$MODEL" \
    --audio-dir ../blinky-test-player/music/edm \
    --output-dir "${EVAL_DIR}/tracks"

# 3. TFLite INT8 export
echo ""
echo "=== TFLite INT8 Export ==="
EXPORT_DIR="${OUTPUT_DIR}/export"
mkdir -p "$EXPORT_DIR"
python scripts/export_tflite.py --config "$CONFIG" --model "$MODEL" \
    --output-dir "$EXPORT_DIR"

echo ""
echo "============================================"
echo "Done! Results in ${OUTPUT_DIR}/"
echo "  Eval:   ${EVAL_DIR}/"
echo "  Export: ${EXPORT_DIR}/"
echo "============================================"
