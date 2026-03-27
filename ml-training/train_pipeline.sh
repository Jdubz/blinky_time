#!/bin/bash
# Efficient ML training pipeline — one command from labels to deployed model.
#
# Usage:
#   ./train_pipeline.sh configs/conv1d_w16_onset_v11.yaml v11-final
#   ./train_pipeline.sh configs/conv1d_w16_onset_v11.yaml v11-final --skip-labels
#   ./train_pipeline.sh configs/conv1d_w16_onset_v11.yaml v11-final --skip-prep
#
# Phases:
#   1. Generate onset labels (skip if --skip-labels or labels exist)
#   2. Prep training data   (skip if --skip-prep or data exists for this config)
#   3. Train model
#   4. Evaluate (threshold sweep on test tracks)
#   5. Export TFLite + C header
#
# Must run inside tmux/screen (train.py enforces this unless --allow-foreground).

set -e

CONFIG="${1:?Usage: $0 <config.yaml> <run-name> [--skip-labels] [--skip-prep]}"
RUN_NAME="${2:?Usage: $0 <config.yaml> <run-name> [--skip-labels] [--skip-prep]}"
SKIP_LABELS=false
SKIP_PREP=false
for arg in "${@:3}"; do
    case "$arg" in
        --skip-labels) SKIP_LABELS=true ;;
        --skip-prep)   SKIP_PREP=true ;;
    esac
done

OUTPUT_DIR="outputs/$RUN_NAME"
DATA_DIR="data/processed"

echo "=== ML Training Pipeline ==="
echo "Config: $CONFIG"
echo "Output: $OUTPUT_DIR"
echo ""

# Phase 1: Onset labels
if [ "$SKIP_LABELS" = false ]; then
    ONSET_DIR=$(python3 -c "import yaml; c=yaml.safe_load(open('$CONFIG')); print(c.get('labels',{}).get('onset_consensus_dir',''))" 2>/dev/null)
    if [ -n "$ONSET_DIR" ]; then
        COUNT=$(ls "$ONSET_DIR"/*.onsets.json 2>/dev/null | wc -l)
        if [ "$COUNT" -lt 6700 ]; then
            echo "=== Phase 1: Generating onset labels ($COUNT/6750 exist) ==="
            python scripts/generate_onset_consensus.py --workers 4
        else
            echo "=== Phase 1: Labels exist ($COUNT files), skipping ==="
        fi
    else
        echo "=== Phase 1: No onset_consensus_dir in config, skipping ==="
    fi
else
    echo "=== Phase 1: Skipped (--skip-labels) ==="
fi

# Phase 2: Data prep
if [ "$SKIP_PREP" = false ]; then
    # Check if data exists and matches config
    if [ -f "$DATA_DIR/X_train.npy" ]; then
        CHUNKS=$(python3 -c "import numpy as np; print(np.load('$DATA_DIR/X_train.npy', mmap_mode='r').shape[0])")
        echo "=== Phase 2: Training data exists ($CHUNKS chunks) ==="
        read -p "Re-prep? (y/N) " -t 10 REPREP || REPREP="n"
        if [ "$REPREP" != "y" ]; then
            echo "  Skipping prep (use --skip-prep to skip without prompt)"
        else
            rm -f "$DATA_DIR"/X_*.npy "$DATA_DIR"/Y_*.npy
            rm -rf "$DATA_DIR/shards"
        fi
    fi

    if [ ! -f "$DATA_DIR/X_train.npy" ]; then
        echo "=== Phase 2: Preparing training data ==="
        # Check if config uses delta features
        USE_DELTA=$(python3 -c "import yaml; c=yaml.safe_load(open('$CONFIG')); print('--delta' if c.get('features',{}).get('use_delta',False) else '')" 2>/dev/null)
        python scripts/prepare_dataset.py --config "$CONFIG" --output-dir "$DATA_DIR" --augment $USE_DELTA
    fi
else
    echo "=== Phase 2: Skipped (--skip-prep) ==="
fi

# Phase 3: Train
echo "=== Phase 3: Training ==="
python train.py --config "$CONFIG" --output-dir "$OUTPUT_DIR" --swa --allow-foreground

# Phase 4: Evaluate
echo "=== Phase 4: Evaluating ==="
python evaluate.py --config "$CONFIG" \
    --model "$OUTPUT_DIR/final_model.pt" \
    --audio-dir ../blinky-test-player/music/edm \
    --output-dir "$OUTPUT_DIR/eval" \
    --sweep-thresholds

# Phase 5: Export
echo "=== Phase 5: Exporting TFLite ==="
python scripts/export_tflite.py --config "$CONFIG" \
    --model "$OUTPUT_DIR/final_model.pt" \
    --output-dir "$OUTPUT_DIR/export"

# Generate C header
python3 -c "
with open('$OUTPUT_DIR/export/frame_onset_model_data_int8.tflite', 'rb') as f:
    data = f.read()
header = '#pragma once\n\n'
header += '// $RUN_NAME onset model\n'
header += f'// {len(data)} bytes INT8\n\n'
header += 'alignas(8) static const unsigned char frame_onset_model_data[] = {\n'
for i in range(0, len(data), 12):
    chunk = data[i:i+12]
    header += '    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',\n'
header += '};\n\n'
header += f'static const unsigned int frame_onset_model_data_len = {len(data)};\n'
with open('../blinky-things/audio/frame_onset_model_data.h', 'w') as f:
    f.write(header)
print(f'C header written: {len(data)} bytes')
"

echo ""
echo "=== Pipeline Complete ==="
echo "Model: $OUTPUT_DIR/final_model.pt"
echo "Eval:  $OUTPUT_DIR/eval/"
echo "TFLite: $OUTPUT_DIR/export/"
echo "Header: ../blinky-things/audio/frame_onset_model_data.h"
echo ""
echo "To deploy: compile firmware and flash devices"
