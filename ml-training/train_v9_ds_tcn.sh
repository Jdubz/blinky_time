#!/bin/bash
# Train v9 DS-TCN variants sequentially on GPU
# v9-24ch: DS-TCN 24ch, 128-frame, with knowledge distillation
# v9-32ch: DS-TCN 32ch, 128-frame, with knowledge distillation
#
# After training, exports TFLite INT8 and runs offline evaluation.

set -euo pipefail
cd "$(dirname "$0")"
source venv/bin/activate

STORAGE=/mnt/storage/blinky-ml-data
TEST_AUDIO=../blinky-test-player/music/edm
LOG="$STORAGE/outputs/v9-ds-tcn-$(date +%Y%m%d-%H%M%S).log"

echo "=== v9 DS-TCN Training Pipeline ===" | tee "$LOG"
echo "Start: $(date)" | tee -a "$LOG"
echo "Log: $LOG" | tee -a "$LOG"

# --- v9-24ch: DS-TCN 24ch with distillation ---
echo "" | tee -a "$LOG"
echo "=== v9-24ch: DS-TCN 24ch (ds_tcn.yaml) ===" | tee -a "$LOG"
V9_24_OUT="$STORAGE/outputs/v9-ds-tcn-24ch"
mkdir -p "$V9_24_OUT"

PYTHONUNBUFFERED=1 python3 train.py \
    --config configs/ds_tcn.yaml \
    --data-dir data/processed \
    --output-dir "$V9_24_OUT" \
    --distill data/processed/Y_teacher_train.npy \
    --distill-alpha 0.3 --distill-temp 2.0 \
    --device cuda 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/ds_tcn.yaml \
    --model "$V9_24_OUT/best_model.pt" \
    --data-dir data/processed \
    --output-dir "$V9_24_OUT" 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/ds_tcn.yaml \
    --model "$V9_24_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V9_24_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "v9-24ch complete: $(date)" | tee -a "$LOG"

# --- v9-32ch: DS-TCN 32ch with distillation ---
echo "" | tee -a "$LOG"
echo "=== v9-32ch: DS-TCN 32ch (ds_tcn_32.yaml) ===" | tee -a "$LOG"
V9_32_OUT="$STORAGE/outputs/v9-ds-tcn-32ch"
mkdir -p "$V9_32_OUT"

PYTHONUNBUFFERED=1 python3 train.py \
    --config configs/ds_tcn_32.yaml \
    --data-dir data/processed \
    --output-dir "$V9_32_OUT" \
    --distill data/processed/Y_teacher_train.npy \
    --distill-alpha 0.3 --distill-temp 2.0 \
    --device cuda 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/ds_tcn_32.yaml \
    --model "$V9_32_OUT/best_model.pt" \
    --data-dir data/processed \
    --output-dir "$V9_32_OUT" 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/ds_tcn_32.yaml \
    --model "$V9_32_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V9_32_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "v9-32ch complete: $(date)" | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== All v9 training complete: $(date) ===" | tee -a "$LOG"
echo "Results:" | tee -a "$LOG"
echo "  v9-24ch: $V9_24_OUT/eval/eval_results.json" | tee -a "$LOG"
echo "  v9-32ch: $V9_32_OUT/eval/eval_results.json" | tee -a "$LOG"
