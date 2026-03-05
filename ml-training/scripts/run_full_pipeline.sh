#!/bin/bash
# Full ML pipeline — runs unattended in background
# Launch with: nohup bash scripts/run_full_pipeline.sh > /mnt/storage/blinky-ml-data/pipeline.log 2>&1 &

set -e
cd /home/jdubz/Development/blinky_time/ml-training
source venv/bin/activate
export TF_USE_LEGACY_KERAS=1

STORAGE="/mnt/storage/blinky-ml-data"
LOG="$STORAGE/pipeline.log"

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

# ============================================================
# Step 1: Wait for FMA download to finish
# ============================================================
log "Step 1: Waiting for FMA download..."
FMA_ZIP="$STORAGE/audio/fma/fma_medium.zip"
while true; do
    if ! pgrep -f "wget.*fma_medium" > /dev/null 2>&1; then
        break
    fi
    SIZE=$(du -h "$FMA_ZIP" 2>/dev/null | awk '{print $1}')
    log "  FMA download: $SIZE / 22G"
    sleep 60
done

if [ ! -f "$FMA_ZIP" ] || [ $(stat -c%s "$FMA_ZIP" 2>/dev/null || echo 0) -lt 20000000000 ]; then
    log "ERROR: FMA download incomplete or missing ($FMA_ZIP). Skipping FMA."
    FMA_OK=0
else
    log "FMA download complete: $(du -h "$FMA_ZIP" | awk '{print $1}')"
    FMA_OK=1
fi

# ============================================================
# Step 2: Extract electronic tracks from FMA
# ============================================================
if [ "$FMA_OK" = "1" ]; then
    log "Step 2: Extracting electronic tracks from FMA..."
    python3 scripts/download_fma.py --output-dir "$STORAGE/audio/fma"
    N_FMA=$(ls "$STORAGE/audio/fma"/*.mp3 2>/dev/null | wc -l)
    log "  Extracted $N_FMA electronic tracks"
fi

# ============================================================
# Step 3: Label FMA tracks with Beat This!
# ============================================================
if [ "$FMA_OK" = "1" ] && [ "$N_FMA" -gt 0 ]; then
    log "Step 3: Labeling FMA tracks with Beat This!..."
    python3 scripts/label_beats.py \
        --audio-dir "$STORAGE/audio/fma" \
        --output-dir "$STORAGE/labels/fma" \
        --device cuda \
        --extensions ".mp3"
    N_LABELS=$(ls "$STORAGE/labels/fma"/*.beats.json 2>/dev/null | wc -l)
    log "  Labeled $N_LABELS FMA tracks"
fi

# ============================================================
# Step 4: Wait for current dataset prep to finish (if running)
# ============================================================
log "Step 4: Waiting for any running prepare_dataset.py..."
while pgrep -f "prepare_dataset.py" > /dev/null 2>&1; do
    sleep 30
done
log "  No prepare_dataset.py running"

# ============================================================
# Step 5: Build combined dataset (test + GiantSteps + FMA)
# ============================================================
log "Step 5: Building combined dataset..."

# Rebuild combined directory with all sources
rm -rf "$STORAGE/audio/combined" "$STORAGE/labels/combined"
mkdir -p "$STORAGE/audio/combined" "$STORAGE/labels/combined"

# Test tracks
ln -sf /home/jdubz/Development/blinky_time/blinky-test-player/music/edm/*.mp3 "$STORAGE/audio/combined/"
ln -sf "$STORAGE/labels/edm-test"/*.beats.json "$STORAGE/labels/combined/"

# GiantSteps
ln -sf "$STORAGE/audio/giantsteps/giantsteps-tempo-dataset/audio"/*.mp3 "$STORAGE/labels/combined/" 2>/dev/null || true
ln -sf "$STORAGE/audio/giantsteps/giantsteps-tempo-dataset/audio"/*.mp3 "$STORAGE/audio/combined/"
ln -sf "$STORAGE/labels/giantsteps"/*.beats.json "$STORAGE/labels/combined/"

# FMA (if available)
if [ "$FMA_OK" = "1" ]; then
    for f in "$STORAGE/audio/fma"/*.mp3; do
        bn=$(basename "$f")
        # Skip the zip file listing artifact
        [ -f "$f" ] && ln -sf "$f" "$STORAGE/audio/combined/$bn"
    done
    for f in "$STORAGE/labels/fma"/*.beats.json; do
        [ -f "$f" ] && ln -sf "$f" "$STORAGE/labels/combined/$(basename "$f")"
    done
fi

N_AUDIO=$(ls "$STORAGE/audio/combined"/*.mp3 2>/dev/null | wc -l)
N_LABELS=$(ls "$STORAGE/labels/combined"/*.beats.json 2>/dev/null | wc -l)
log "  Combined: $N_AUDIO audio files, $N_LABELS labels"

# ============================================================
# Step 6: Prepare dataset with augmentation
# ============================================================
log "Step 6: Preparing augmented dataset..."
python3 scripts/prepare_dataset.py \
    --config configs/default.yaml \
    --audio-dir "$STORAGE/audio/combined" \
    --labels-dir "$STORAGE/labels/combined" \
    --output-dir data/processed \
    --augment \
    --rir-dir "$STORAGE/rir/processed"
log "  Dataset preparation complete"

# ============================================================
# Step 7: Train model
# ============================================================
log "Step 7: Training model..."
CUDA_VISIBLE_DEVICES="" python3 train.py \
    --config configs/default.yaml \
    --data-dir data/processed \
    --output-dir outputs/full-v1 \
    --epochs 100
log "  Training complete"

# ============================================================
# Step 8: Export TFLite INT8
# ============================================================
log "Step 8: Exporting TFLite INT8..."
CUDA_VISIBLE_DEVICES="" python3 scripts/export_tflite.py \
    --config configs/default.yaml \
    --model outputs/full-v1/best_model.keras \
    --data-dir data/processed \
    --output-dir outputs/full-v1
log "  Export complete"

# ============================================================
# Step 9: Evaluate on test tracks
# ============================================================
log "Step 9: Evaluating on test tracks..."
CUDA_VISIBLE_DEVICES="" python3 evaluate.py \
    --config configs/default.yaml \
    --model outputs/full-v1/best_model.keras \
    --audio-dir ../blinky-test-player/music/edm \
    --output-dir outputs/full-v1/eval
log "  Evaluation complete"

log "============================================================"
log "PIPELINE COMPLETE"
log "Results in: outputs/full-v1/"
log "  Model:  outputs/full-v1/best_model.keras"
log "  TFLite: outputs/full-v1/beat_model_int8.tflite"
log "  Eval:   outputs/full-v1/eval/"
log "  Log:    outputs/full-v1/training_log.csv"
log "============================================================"
