#!/bin/bash
# Train v6-restart, v7, v8 sequentially on GPU
# v6: 5L ch32, 128-frame, data/processed/
# v7: 7L ch32, 256-frame, data/processed-deep/
# v8: 7L ch48, 256-frame, data/processed-deep/

set -euo pipefail
cd "$(dirname "$0")"
source venv/bin/activate

STORAGE=/mnt/storage/blinky-ml-data
TEST_AUDIO=../blinky-test-player/music/edm
LOG="$STORAGE/outputs/all-training-$(date +%Y%m%d-%H%M%S).log"

echo "=== Training Pipeline ===" | tee "$LOG"
echo "Start: $(date)" | tee -a "$LOG"
echo "Log: $LOG" | tee -a "$LOG"

# --- v6-restart: 5L ch32 with all pipeline fixes ---
echo "" | tee -a "$LOG"
echo "=== v6-restart: 5L ch32 (wider_rf.yaml) ===" | tee -a "$LOG"
V6_OUT="$STORAGE/outputs/v6-restart"
mkdir -p "$V6_OUT"

python3 train.py \
    --config configs/wider_rf.yaml \
    --data-dir data/processed \
    --output-dir "$V6_OUT" \
    --epochs 100 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/wider_rf.yaml \
    --model "$V6_OUT/best_model.pt" \
    --data-dir data/processed \
    --output-dir "$V6_OUT" 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/wider_rf.yaml \
    --model "$V6_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V6_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "v6-restart complete: $(date)" | tee -a "$LOG"

# --- v7: 7L ch32 (deep.yaml) ---
echo "" | tee -a "$LOG"
echo "=== v7: 7L ch32 (deep.yaml) ===" | tee -a "$LOG"
V7_OUT="$STORAGE/outputs/v7-deep-ch32"
mkdir -p "$V7_OUT"

python3 train.py \
    --config configs/deep.yaml \
    --data-dir data/processed-deep \
    --output-dir "$V7_OUT" \
    --epochs 100 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/deep.yaml \
    --model "$V7_OUT/best_model.pt" \
    --data-dir data/processed-deep \
    --output-dir "$V7_OUT" 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/deep.yaml \
    --model "$V7_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V7_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "v7 complete: $(date)" | tee -a "$LOG"

# --- v8: 7L ch48 (deep_wide.yaml) ---
echo "" | tee -a "$LOG"
echo "=== v8: 7L ch48 (deep_wide.yaml) ===" | tee -a "$LOG"
V8_OUT="$STORAGE/outputs/v8-deep-wide-ch48"
mkdir -p "$V8_OUT"

python3 train.py \
    --config configs/deep_wide.yaml \
    --data-dir data/processed-deep \
    --output-dir "$V8_OUT" \
    --epochs 100 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/deep_wide.yaml \
    --model "$V8_OUT/best_model.pt" \
    --data-dir data/processed-deep \
    --output-dir "$V8_OUT" 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/deep_wide.yaml \
    --model "$V8_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V8_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "v8 complete: $(date)" | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== All training complete: $(date) ===" | tee -a "$LOG"
echo "Results:" | tee -a "$LOG"
echo "  v6-restart: $V6_OUT/eval/eval_results.json" | tee -a "$LOG"
echo "  v7:         $V7_OUT/eval/eval_results.json" | tee -a "$LOG"
echo "  v8:         $V8_OUT/eval/eval_results.json" | tee -a "$LOG"
