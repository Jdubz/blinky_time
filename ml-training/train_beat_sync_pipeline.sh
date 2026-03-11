#!/bin/bash
# Beat-synchronous classifier training pipeline (Phase A: downbeat only)
#
# Steps:
#   1. Extract beat features from audio + labels (GPU)
#   2. Train BeatSyncClassifier (GPU)
#   3. Export to TFLite INT8 + C header (CPU)
#
# Usage:
#   ./train_beat_sync_pipeline.sh              # Full pipeline
#   ./train_beat_sync_pipeline.sh --skip-extract  # Skip feature extraction (reuse existing)

set -euo pipefail
cd "$(dirname "$0")"
source venv/bin/activate

PHASE="A"
CONFIG="configs/beat_sync.yaml"
DATA_DIR="data/beat_sync"
STORAGE=/mnt/storage/blinky-ml-data
OUTPUT_DIR="$STORAGE/outputs/beat-sync-phase${PHASE}-$(date +%Y%m%d-%H%M%S)"
LOG="$STORAGE/outputs/beat-sync-phase${PHASE}-$(date +%Y%m%d-%H%M%S).log"

SKIP_EXTRACT=false
for arg in "$@"; do
    case "$arg" in
        --skip-extract) SKIP_EXTRACT=true ;;
    esac
done

echo "=== Beat-Sync Phase $PHASE Training Pipeline ===" | tee "$LOG"
echo "Start: $(date)" | tee -a "$LOG"
echo "Config: $CONFIG" | tee -a "$LOG"
echo "Output: $OUTPUT_DIR" | tee -a "$LOG"
echo "Log: $LOG" | tee -a "$LOG"

# --- Step 1: Extract beat features ---
if [ "$SKIP_EXTRACT" = false ]; then
    echo "" | tee -a "$LOG"
    echo "=== Step 1: Extracting beat features ===" | tee -a "$LOG"
    PYTHONUNBUFFERED=1 python scripts/beat_feature_extractor.py \
        --config "$CONFIG" \
        --output-dir "$DATA_DIR" \
        --augment \
        --device cuda 2>&1 | tee -a "$LOG"
else
    echo "" | tee -a "$LOG"
    echo "=== Step 1: SKIPPED (--skip-extract) ===" | tee -a "$LOG"
fi

# --- Step 2: Train ---
echo "" | tee -a "$LOG"
echo "=== Step 2: Training BeatSyncClassifier (phase=$PHASE) ===" | tee -a "$LOG"
mkdir -p "$OUTPUT_DIR"

PYTHONUNBUFFERED=1 python train_beat_sync.py \
    --config "$CONFIG" \
    --data-dir "$DATA_DIR" \
    --output-dir "$OUTPUT_DIR" \
    --phase "$PHASE" \
    --device cuda 2>&1 | tee -a "$LOG"

# --- Step 3: Export ---
echo "" | tee -a "$LOG"
echo "=== Step 3: Exporting to TFLite INT8 ===" | tee -a "$LOG"

python scripts/export_beat_sync.py \
    --config "$CONFIG" \
    --model "$OUTPUT_DIR/best_model.pt" \
    --output-dir "$OUTPUT_DIR" \
    --data-dir "$DATA_DIR" \
    --phase "$PHASE" 2>&1 | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== Pipeline complete: $(date) ===" | tee -a "$LOG"
echo "Results: $OUTPUT_DIR/" | tee -a "$LOG"
echo "  Model:  $OUTPUT_DIR/best_model.pt" | tee -a "$LOG"
echo "  TFLite: $OUTPUT_DIR/beat_sync_int8.tflite" | tee -a "$LOG"
echo "  Eval:   $OUTPUT_DIR/eval_results.json" | tee -a "$LOG"
