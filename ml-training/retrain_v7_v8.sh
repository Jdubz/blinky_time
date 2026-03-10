#!/bin/bash
# Retrain v7 and v8 with fixed mel filterbank (weighted average normalization).
#
# Steps:
#   1. Re-prepare deep data (256-frame chunks) with fixed mel pipeline
#   2. Train v7 (7L ch32) and v8 (7L ch48) in parallel on RTX 3080
#   3. Export both to TFLite INT8
#   4. Evaluate both on EDM test set
#
# Both models share the same processed data (data/processed-deep-v2/).

set -euo pipefail
cd "$(dirname "$0")"
source venv/bin/activate

STORAGE=/mnt/storage/blinky-ml-data
AUDIO_DIR="$STORAGE/audio/combined"
LABELS_DIR="$STORAGE/labels/consensus_v2"
RIR_DIR="$STORAGE/rir/processed"
TEST_AUDIO="../blinky-test-player/music/edm"
PROCESSED_DIR="data/processed-deep-v2"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LOG="$STORAGE/outputs/retrain-v7v8-$TIMESTAMP.log"

V7_OUT="$STORAGE/outputs/v7-melfixed-$TIMESTAMP"
V8_OUT="$STORAGE/outputs/v8-melfixed-$TIMESTAMP"

echo "=== Retrain v7 + v8 (mel filterbank fix) ===" | tee "$LOG"
echo "Start: $(date)" | tee -a "$LOG"
echo "Log: $LOG" | tee -a "$LOG"
echo "V7 output: $V7_OUT" | tee -a "$LOG"
echo "V8 output: $V8_OUT" | tee -a "$LOG"

# --- Step 1: Re-prepare deep data with fixed mel pipeline ---
echo "" | tee -a "$LOG"
echo "=== Step 1: Preparing data (256-frame, fixed mel filterbank) ===" | tee -a "$LOG"
python3 scripts/prepare_dataset.py \
    --config configs/deep.yaml \
    --audio-dir "$AUDIO_DIR" \
    --labels-dir "$LABELS_DIR" \
    --output-dir "$PROCESSED_DIR" \
    --rir-dir "$RIR_DIR" \
    --exclude-dir "$TEST_AUDIO" \
    --augment 2>&1 | tee -a "$LOG"

echo "Data prep complete: $(date)" | tee -a "$LOG"

# --- Step 2: Train v7 and v8 in parallel ---
# Both use the same processed data but different configs.
# Batch size 32 to fit both on 10 GB RTX 3080 simultaneously.
echo "" | tee -a "$LOG"
echo "=== Step 2: Training v7 and v8 in parallel ===" | tee -a "$LOG"

mkdir -p "$V7_OUT" "$V8_OUT"

# v7: 7L ch32
(
    echo "[v7] Training started: $(date)"
    python3 train.py \
        --config configs/deep.yaml \
        --data-dir "$PROCESSED_DIR" \
        --output-dir "$V7_OUT" \
        --epochs 100 \
        --batch-size 32 2>&1
    echo "[v7] Training complete: $(date)"
) > >(tee -a "$V7_OUT/train.log") 2>&1 &
V7_PID=$!

# v8: 7L ch48
(
    echo "[v8] Training started: $(date)"
    python3 train.py \
        --config configs/deep_wide.yaml \
        --data-dir "$PROCESSED_DIR" \
        --output-dir "$V8_OUT" \
        --epochs 100 \
        --batch-size 32 2>&1
    echo "[v8] Training complete: $(date)"
) > >(tee -a "$V8_OUT/train.log") 2>&1 &
V8_PID=$!

echo "v7 PID: $V7_PID, v8 PID: $V8_PID" | tee -a "$LOG"
echo "Waiting for both to complete..." | tee -a "$LOG"

# Wait for both, capture exit codes
V7_EXIT=0
V8_EXIT=0
wait $V7_PID || V7_EXIT=$?
wait $V8_PID || V8_EXIT=$?

echo "" | tee -a "$LOG"
echo "v7 exit: $V7_EXIT, v8 exit: $V8_EXIT" | tee -a "$LOG"

if [ $V7_EXIT -ne 0 ] || [ $V8_EXIT -ne 0 ]; then
    echo "ERROR: Training failed (v7=$V7_EXIT, v8=$V8_EXIT)" | tee -a "$LOG"
    echo "Check logs: $V7_OUT/train.log, $V8_OUT/train.log" | tee -a "$LOG"
    exit 1
fi

# --- Step 3: Export TFLite INT8 ---
echo "" | tee -a "$LOG"
echo "=== Step 3: Exporting TFLite INT8 ===" | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/deep.yaml \
    --model "$V7_OUT/best_model.pt" \
    --data-dir "$PROCESSED_DIR" \
    --output-dir "$V7_OUT" 2>&1 | tee -a "$LOG"

python3 scripts/export_tflite.py \
    --config configs/deep_wide.yaml \
    --model "$V8_OUT/best_model.pt" \
    --data-dir "$PROCESSED_DIR" \
    --output-dir "$V8_OUT" 2>&1 | tee -a "$LOG"

# --- Step 4: Evaluate on test tracks ---
echo "" | tee -a "$LOG"
echo "=== Step 4: Evaluating on test tracks ===" | tee -a "$LOG"

python3 evaluate.py \
    --config configs/deep.yaml \
    --model "$V7_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V7_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

python3 evaluate.py \
    --config configs/deep_wide.yaml \
    --model "$V8_OUT/best_model.pt" \
    --audio-dir "$TEST_AUDIO" \
    --output-dir "$V8_OUT/eval" \
    --sweep-thresholds 2>&1 | tee -a "$LOG"

echo "" | tee -a "$LOG"
echo "=== All complete: $(date) ===" | tee -a "$LOG"
echo "Results:" | tee -a "$LOG"
echo "  v7: $V7_OUT/eval/eval_results.json" | tee -a "$LOG"
echo "  v8: $V8_OUT/eval/eval_results.json" | tee -a "$LOG"
echo "  v7 TFLite: $V7_OUT/beat_model_int8.tflite" | tee -a "$LOG"
echo "  v8 TFLite: $V8_OUT/beat_model_int8.tflite" | tee -a "$LOG"
