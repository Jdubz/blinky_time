#!/bin/bash
# Train v9 onset model with knowledge distillation + tempo auxiliary head
#
# Run in tmux: tmux new-session -d -s v9 "cd ml-training && bash train_v9.sh"
# Monitor:     tmux attach -t v9
# Log:         tail -f outputs/conv1d_w16_onset_v9/training.log
#
# Prerequisites:
#   - venv activated (source venv/bin/activate)
#   - Data prepared (data/processed/X_train.npy exists)
#   - GPU available (nvidia-smi)
#
# Steps:
#   1. Generate teacher labels from Beat This! (if not cached)
#   2. Train with distillation + tempo auxiliary head
#   3. Export to TFLite INT8
#   4. Evaluate on test set

set -eo pipefail

CONFIG="configs/conv1d_w16_onset_v9.yaml"
OUTPUT_DIR="outputs/conv1d_w16_onset_v9"
TEACHER_TRAIN="data/processed/Y_teacher_train.npy"
TEACHER_VAL="data/processed/Y_teacher_val.npy"

echo "=== v9 Onset Model Training ==="
echo "Config: $CONFIG"
echo "Output: $OUTPUT_DIR"
echo ""

# Activate venv
source venv/bin/activate

# Check GPU
python -c "import torch; print(f'GPU: {torch.cuda.get_device_name(0) if torch.cuda.is_available() else \"NONE\"}')"

# Check data exists
if [ ! -f "data/processed/X_train.npy" ]; then
    echo "ERROR: Training data not found. Run prepare_dataset.py first:"
    echo "  python scripts/prepare_dataset.py --config $CONFIG --augment"
    exit 1
fi

# Swap to onset-only labels (2D, shape=(N,128)) before anything else.
# The v8 dataset prep generated 3-channel instrument labels (N,128,3).
# v9 needs 1-channel onset labels (N,128) for both training and teacher generation.
if [ -f "data/processed/Y_onset_train.npy" ]; then
    echo "Swapping to onset-only labels..."
    # Backup original 3-channel labels (only if not already backed up)
    if [ ! -L "data/processed/Y_train.npy" ]; then
        mv data/processed/Y_train.npy data/processed/Y_instrument_train.npy 2>/dev/null || true
        mv data/processed/Y_val.npy data/processed/Y_instrument_val.npy 2>/dev/null || true
    fi
    ln -sf Y_onset_train.npy data/processed/Y_train.npy
    ln -sf Y_onset_val.npy data/processed/Y_val.npy
    echo "  Y_train.npy → Y_onset_train.npy (shape: N×128)"
fi

# Step 1: Generate teacher labels (Gaussian-smoothed hard labels, sigma=3.0)
if [ ! -f "$TEACHER_TRAIN" ] || [ ! -f "$TEACHER_VAL" ]; then
    echo ""
    echo "=== Step 1: Generating teacher labels ==="
    python scripts/generate_teacher_labels.py --data-dir data/processed
    echo "Teacher labels generated."
else
    echo "Teacher labels already exist, skipping generation."
fi

# Step 2: Train
echo ""
echo "=== Step 2: Training with distillation + tempo head ==="
PYTHONUNBUFFERED=1 python train.py \
    --config "$CONFIG" \
    --output-dir "$OUTPUT_DIR" \
    --distill "$TEACHER_TRAIN" \
    --allow-foreground \
    2>&1 | tee "$OUTPUT_DIR/training.log"

# Step 3: Export
echo ""
echo "=== Step 3: Exporting to TFLite INT8 ==="
python scripts/export_tflite.py \
    --config "$CONFIG" \
    --checkpoint "$OUTPUT_DIR/best_model.pt" \
    --output "$OUTPUT_DIR/export"

# Step 4: Evaluate
echo ""
echo "=== Step 4: Evaluating ==="
python evaluate.py \
    --config "$CONFIG" \
    --checkpoint "$OUTPUT_DIR/best_model.pt" \
    --output-dir "$OUTPUT_DIR/eval"

# Restore original 3-channel labels if we swapped them
if [ -f "data/processed/Y_instrument_train.npy" ]; then
    echo "Restoring original 3-channel labels..."
    rm -f data/processed/Y_train.npy data/processed/Y_val.npy
    mv data/processed/Y_instrument_train.npy data/processed/Y_train.npy
    mv data/processed/Y_instrument_val.npy data/processed/Y_val.npy
fi

echo ""
echo "=== Training complete ==="
echo "Model: $OUTPUT_DIR/best_model.pt"
echo "TFLite: $OUTPUT_DIR/export/"
echo "Evaluation: $OUTPUT_DIR/eval/"
