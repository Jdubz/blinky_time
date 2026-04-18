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
# train.py uses --allow-foreground since this pipeline is designed
# to be launched via tmux/nohup (the caller manages session persistence).

set -e

# Allow PyTorch CUDA allocator to use non-contiguous segments for large tensors.
# Without this, fragmentation after hundreds of tracks leaves no contiguous block
# for FFT intermediates on long tracks, causing unrecoverable OOM.
export PYTORCH_CUDA_ALLOC_CONF="expandable_segments:True"

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
mkdir -p "$OUTPUT_DIR"  # Create early so tee can write the log from the start

# Read processed_dir from config — fail fast on parse errors
DATA_DIR=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('data',{}).get('processed_dir','data/processed'))" "$CONFIG")
if [ -z "$DATA_DIR" ]; then
    echo "WARNING: Could not read data.processed_dir from $CONFIG, using default data/processed"
    DATA_DIR="data/processed"
fi

echo "=== ML Training Pipeline ==="
echo "Config: $CONFIG"
echo "Output: $OUTPUT_DIR"
echo "Data:   $DATA_DIR"
echo ""

# Phase 1: Onset labels
if [ "$SKIP_LABELS" = false ]; then
    ONSET_DIR=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('labels',{}).get('onset_consensus_dir',''))" "$CONFIG" 2>/dev/null)
    if [ -n "$ONSET_DIR" ]; then
        COUNT=$(find "$ONSET_DIR" -name '*.onsets.json' -maxdepth 1 2>/dev/null | wc -l)
        EXPECTED=$(find "$ONSET_DIR"/.. -path '*/consensus_v5/*.beats.json' -maxdepth 2 2>/dev/null | wc -l)
        EXPECTED=${EXPECTED:-6750}
        if [ "$COUNT" -lt "$EXPECTED" ]; then
            echo "=== Phase 1: Generating onset labels ($COUNT/$EXPECTED exist) ==="
            python scripts/generate_onset_consensus.py --output-dir "$ONSET_DIR" --workers 4
        else
            echo "=== Phase 1: Labels exist ($COUNT/$EXPECTED files), skipping ==="
        fi
    else
        echo "=== Phase 1: No onset_consensus_dir in config, skipping ==="
    fi
else
    echo "=== Phase 1: Skipped (--skip-labels) ==="
fi

# Phase 1b: Validate kick_weighted labels (if applicable)
KW_DIR=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('labels',{}).get('kick_weighted_dir',''))" "$CONFIG" 2>/dev/null)
LABELS_TYPE=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('labels',{}).get('labels_type',''))" "$CONFIG" 2>/dev/null)
if [ -n "$KW_DIR" ] && [ "$LABELS_TYPE" = "kick_weighted" ]; then
    echo "=== Phase 1b: Validating kick-weighted labels ==="
    CONSENSUS_DIR=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('labels',{}).get('labels_dir', '/mnt/storage/blinky-ml-data/labels/consensus_v5'))" "$CONFIG" 2>/dev/null)
    python scripts/validate_kick_weighted.py --label-dir "$KW_DIR" --consensus-dir "$CONSENSUS_DIR" --quiet || {
        echo "  Label validation found issues (see above). Continuing with training."
    }
fi

# Phase 2: Data prep
if [ "$SKIP_PREP" = false ]; then
    # Check if data exists and warn if it might not match current config
    if [ -f "$DATA_DIR/X_train.npy" ]; then
        CHUNKS=$(python3 -c "import numpy as np; print(np.load('$DATA_DIR/X_train.npy', mmap_mode='r').shape[0])")
        echo "=== Phase 2: Training data exists ($CHUNKS chunks) ==="
        # Warn if data was built from a different config
        if [ -f "$DATA_DIR/.prep_config" ]; then
            PREV_CONFIG=$(cat "$DATA_DIR/.prep_config")
            if [ "$PREV_CONFIG" != "$CONFIG" ]; then
                echo "  WARNING: Data was prepped with '$PREV_CONFIG', not '$CONFIG'"
            fi
        else
            echo "  WARNING: Cannot verify data matches config (no .prep_config marker)"
        fi
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
        USE_DELTA=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print('--delta' if c.get('features',{}).get('use_delta',False) else '')" "$CONFIG" 2>/dev/null)
        # Note: prepare_dataset.py may report "VALIDATION FAILED" for mel range
        # when delta features are enabled (deltas can be negative). This is expected.
        # Apply mic profile if available (transforms clean mel → mic-captured equivalent)
        MIC_PROFILE=""
        if [ -f "data/calibration/mic_profile.npz" ]; then
            MIC_PROFILE="--mic-profile data/calibration/mic_profile.npz"
        fi
        TQDM_DISABLE=1 python scripts/prepare_dataset.py --config "$CONFIG" --output-dir "$DATA_DIR" --augment $USE_DELTA $MIC_PROFILE || {
            # Check if data was actually written despite validation warning
            if [ -f "$DATA_DIR/X_train.npy" ]; then
                echo "  (Data prep completed with warnings — continuing)"
            else
                echo "  DATA PREP FAILED — aborting"
                exit 1
            fi
        }
        # Record which config was used for this data prep
        echo "$CONFIG" > "$DATA_DIR/.prep_config"
    fi
else
    echo "=== Phase 2: Skipped (--skip-prep) ==="
fi

# Phase 3: Train
echo "=== Phase 3: Training ==="
python train.py --config "$CONFIG" --output-dir "$OUTPUT_DIR" --allow-foreground

# Phase 4: Evaluate (use best_model.pt — SWA/final_model.pt proven unhelpful)
echo "=== Phase 4: Evaluating ==="
python evaluate.py --config "$CONFIG" \
    --model "$OUTPUT_DIR/best_model.pt" \
    --audio-dir ../blinky-test-player/music/edm \
    --output-dir "$OUTPUT_DIR/eval" \
    --sweep-thresholds

# Phase 5: Export
echo "=== Phase 5: Exporting TFLite ==="
python scripts/export_tflite.py --config "$CONFIG" \
    --model "$OUTPUT_DIR/best_model.pt" \
    --output-dir "$OUTPUT_DIR/export"

# C header is generated by export_tflite.py (reads output_header from config)
HEADER_PATH=$(python3 -c "import sys, yaml; c=yaml.safe_load(open(sys.argv[1])); print(c.get('export',{}).get('output_header',''))" "$CONFIG" 2>/dev/null)

echo ""
echo "=== Pipeline Complete ==="
echo "Model: $OUTPUT_DIR/best_model.pt"
echo "Eval:  $OUTPUT_DIR/eval/"
echo "TFLite: $OUTPUT_DIR/export/"
echo "Header: $HEADER_PATH"
echo ""
echo "To deploy: compile firmware and flash devices"
