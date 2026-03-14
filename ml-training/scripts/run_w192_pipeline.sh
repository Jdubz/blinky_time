#!/bin/bash
# W192 Training Pipeline — run each step sequentially after GPU is free.
#
# This script runs inside tmux. Each step checks the previous step's output
# before proceeding. If any step fails, the pipeline stops.
#
# Prerequisites:
#   - Preprocessing must be complete (data/processed/X_train.npy exists)
#   - Run via: tmux new-session -d -s pipeline "cd ml-training && bash scripts/run_w192_pipeline.sh 2>&1 | tee outputs/pipeline.log"
#
# Steps:
#   1. Train W192 model on current data (consensus_v4, full-mix only)
#   2. Export + evaluate W192 model
#   3. Batch Demucs separation (all 7000 tracks → stems)
#   4. Complete allin1 labeling (using cached stems)
#   5. Build consensus_v5 (7 systems)
#   6. Re-prep dataset with stems + consensus_v5
#   7. Retrain W192 on improved data
#
# The pipeline can be resumed — each step checks for existing output.

set -eo pipefail

cd "$(dirname "$0")/.."
source venv/bin/activate

OUTPUTS="outputs"
DATA_ROOT="${BLINKY_DATA_ROOT:-/mnt/storage/blinky-ml-data}"
STEMS_DIR="$DATA_ROOT/stems"
LABELS_DIR="$DATA_ROOT/labels/multi"
CONSENSUS_V5_DIR="$DATA_ROOT/labels/consensus_v5"
AUDIO_DIR="$DATA_ROOT/audio/combined"

echo "============================================================"
echo "  W192 Training Pipeline"
echo "  Started: $(date)"
echo "============================================================"
echo ""

# ──────────────────────────────────────────────────────────────────
# Preflight checks
# ──────────────────────────────────────────────────────────────────
echo "[Preflight] Checking prerequisites..."
if [ ! -f "data/processed/X_train.npy" ]; then
    echo "  ERROR: data/processed/X_train.npy not found."
    echo "  Preprocessing must complete before running this pipeline."
    echo "  Check: tmux attach -t preprocess"
    exit 1
fi
echo "  Processed data: OK"

if [ ! -d "$AUDIO_DIR" ]; then
    echo "  ERROR: Audio directory not found: $AUDIO_DIR"
    exit 1
fi
TOTAL_AUDIO=$(find "$AUDIO_DIR" -maxdepth 1 -name "*.mp3" | wc -l)
echo "  Audio tracks: $TOTAL_AUDIO"

if [ ! -f "data/calibration/mic_profile.npz" ]; then
    echo "  ERROR: Mic profile not found: data/calibration/mic_profile.npz"
    exit 1
fi
echo "  Mic profile: OK"

# Check disk space (need ~70 GB for stems + ~130 GB for reprocessed data)
STORAGE_FREE_KB=$(df --output=avail /mnt/storage | tail -1)
NVMe_FREE_KB=$(df --output=avail /home | tail -1)
echo "  Storage free: $((STORAGE_FREE_KB / 1024 / 1024)) GB (need ~70 GB for stems)"
echo "  NVMe free: $((NVMe_FREE_KB / 1024 / 1024)) GB (need ~130 GB for data)"

if [ "$STORAGE_FREE_KB" -lt 75000000 ]; then
    echo "  WARNING: Less than 75 GB free on /mnt/storage — stems may not fit"
fi
if [ "$NVMe_FREE_KB" -lt 140000000 ]; then
    echo "  WARNING: Less than 140 GB free on NVMe — reprocessed data may not fit"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 1: Train W192 on current data
# ──────────────────────────────────────────────────────────────────
echo "[Step 1/7] Train W192 model (consensus_v4, full-mix)"
echo "  Started: $(date)"
if [ -f "$OUTPUTS/w192/best_model.pt" ]; then
    echo "  SKIP: $OUTPUTS/w192/best_model.pt already exists"
else
    mkdir -p "$OUTPUTS/w192"
    PYTHONUNBUFFERED=1 python train.py \
        --config configs/frame_fc_w192.yaml \
        --output-dir "$OUTPUTS/w192"
    echo "  DONE: Training complete at $(date)"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 2: Export + evaluate W192
# ──────────────────────────────────────────────────────────────────
echo "[Step 2/7] Export + evaluate W192"
if [ -f "$OUTPUTS/w192/export/frame_beat_model_data_int8.tflite" ]; then
    echo "  SKIP: TFLite model already exported"
else
    mkdir -p "$OUTPUTS/w192/export"
    python scripts/export_tflite.py \
        --config configs/frame_fc_w192.yaml \
        --model "$OUTPUTS/w192/best_model.pt" \
        --output-dir "$OUTPUTS/w192/export"
    echo "  DONE: Export complete"
fi

if [ -f "$OUTPUTS/w192/eval/eval_results.json" ]; then
    echo "  SKIP: Evaluation already complete"
else
    mkdir -p "$OUTPUTS/w192/eval"
    python evaluate.py \
        --config configs/frame_fc_w192.yaml \
        --model "$OUTPUTS/w192/best_model.pt" \
        --audio-dir ../blinky-test-player/music/edm \
        --output-dir "$OUTPUTS/w192/eval"
    echo "  DONE: Evaluation complete"
fi

# Print eval summary
if [ -f "$OUTPUTS/w192/eval/eval_results.json" ]; then
    python3 -c "
import json
with open('$OUTPUTS/w192/eval/eval_results.json') as f:
    data = json.load(f)
f1s = [t['f1'] for t in data]
db = [t['db_f1'] for t in data]
print(f'  W192 baseline: Beat F1={sum(f1s)/len(f1s):.3f}, DB F1={sum(db)/len(db):.3f} ({len(data)} tracks)')
"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 3: Batch Demucs separation
# ──────────────────────────────────────────────────────────────────
echo "[Step 3/7] Batch Demucs source separation"
echo "  Started: $(date)"
DONE_STEMS=$(find "$STEMS_DIR/htdemucs" -name "drums.wav" 2>/dev/null | wc -l)
echo "  Tracks: $TOTAL_AUDIO total, $DONE_STEMS already separated"
if [ "$DONE_STEMS" -ge "$TOTAL_AUDIO" ]; then
    echo "  SKIP: All tracks already separated"
else
    PYTHONUNBUFFERED=1 python scripts/batch_demucs_separate.py \
        --audio-dir "$AUDIO_DIR" \
        --output-dir "$STEMS_DIR" \
        --device cuda
    echo "  DONE: Demucs separation complete at $(date)"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 4: Complete allin1 labeling (using cached stems)
# ──────────────────────────────────────────────────────────────────
echo "[Step 4/7] allin1 labeling"
echo "  Started: $(date)"
ALLIN1_DONE=$(find "$LABELS_DIR" -maxdepth 1 -name "*.allin1.beats.json" 2>/dev/null | wc -l)
echo "  allin1 labels: $ALLIN1_DONE / $TOTAL_AUDIO"
if [ "$ALLIN1_DONE" -ge "$TOTAL_AUDIO" ]; then
    echo "  SKIP: All tracks already labeled"
else
    # Pass --demix-dir pointing to stems from Step 3.
    # allin1's demix() looks for {demix_dir}/htdemucs/{track}/ — our
    # batch_demucs_separate.py saves to {STEMS_DIR}/htdemucs/{track}/,
    # so passing STEMS_DIR as demix_dir makes the paths align.
    # --allin1-device cuda ensures NN inference runs on GPU (~1s vs 86s CPU).
    PYTHONUNBUFFERED=1 python scripts/label_beats.py \
        --audio-dir "$AUDIO_DIR" \
        --output-dir "$LABELS_DIR" \
        --systems allin1 \
        --demix-dir "$STEMS_DIR" \
        --allin1-device cuda \
        --workers 1
    echo "  DONE: allin1 labeling complete at $(date)"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 5: Build consensus_v5 (7 systems)
# ──────────────────────────────────────────────────────────────────
echo "[Step 5/7] Build consensus_v5 labels"
V5_COUNT=$(find "$CONSENSUS_V5_DIR" -maxdepth 1 -name "*.beats.json" 2>/dev/null | wc -l)
if [ "$V5_COUNT" -gt 6000 ]; then
    echo "  SKIP: consensus_v5 already exists ($V5_COUNT labels)"
else
    mkdir -p "$CONSENSUS_V5_DIR"
    PYTHONUNBUFFERED=1 python scripts/merge_consensus_labels_v2.py \
        --labels-dir "$LABELS_DIR" \
        --output-dir "$CONSENSUS_V5_DIR" \
        --tolerance 0.05 \
        --min-agreement 2 \
        --downbeat-min-agreement 2
    echo "  DONE: consensus_v5 built"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 6: Re-prep dataset with stems + consensus_v5
# ──────────────────────────────────────────────────────────────────
echo "[Step 6/7] Prepare dataset (consensus_v5 + drum stem augmentation)"
echo "  Started: $(date)"
# Move old processed data to backup (recoverable if re-prep fails)
if [ -f "data/processed/X_train.npy" ]; then
    BACKUP_DIR="data/processed_backup_$(date +%Y%m%d_%H%M%S)"
    echo "  Backing up old data to $BACKUP_DIR..."
    mv data/processed "$BACKUP_DIR"
    mkdir -p data/processed
fi

PYTHONUNBUFFERED=1 python scripts/prepare_dataset.py \
    --config configs/frame_fc_w192.yaml \
    --augment \
    --labels-dir "$CONSENSUS_V5_DIR" \
    --mic-profile data/calibration/mic_profile.npz \
    --exclude-dir ../blinky-test-player/music/edm \
    --rir-dir /mnt/storage/blinky-ml-data/rir/processed \
    --stems-dir "$STEMS_DIR" \
    --stem-variants drums
echo "  DONE: Dataset prepared at $(date)"
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 7: Retrain W192 on improved data
# ──────────────────────────────────────────────────────────────────
echo "[Step 7/7] Retrain W192 (consensus_v5 + drum stems)"
echo "  Started: $(date)"
mkdir -p "$OUTPUTS/w192_v5_stems"
PYTHONUNBUFFERED=1 python train.py \
    --config configs/frame_fc_w192.yaml \
    --output-dir "$OUTPUTS/w192_v5_stems"
echo "  DONE: Retraining complete at $(date)"

# Final export + eval
echo ""
echo "[Final] Export + evaluate retrained model"
mkdir -p "$OUTPUTS/w192_v5_stems/export" "$OUTPUTS/w192_v5_stems/eval"
python scripts/export_tflite.py \
    --config configs/frame_fc_w192.yaml \
    --model "$OUTPUTS/w192_v5_stems/best_model.pt" \
    --output-dir "$OUTPUTS/w192_v5_stems/export"

python evaluate.py \
    --config configs/frame_fc_w192.yaml \
    --model "$OUTPUTS/w192_v5_stems/best_model.pt" \
    --audio-dir ../blinky-test-player/music/edm \
    --output-dir "$OUTPUTS/w192_v5_stems/eval"

# Print comparison
echo ""
echo "============================================================"
echo "  Pipeline Complete: $(date)"
echo "============================================================"
echo ""
python3 -c "
import json
for name, path in [('W192 baseline', '$OUTPUTS/w192/eval/eval_results.json'),
                   ('W192 v5+stems', '$OUTPUTS/w192_v5_stems/eval/eval_results.json')]:
    try:
        with open(path) as f:
            data = json.load(f)
        f1s = [t['f1'] for t in data]
        db = [t['db_f1'] for t in data]
        print(f'  {name}: Beat F1={sum(f1s)/len(f1s):.3f}, DB F1={sum(db)/len(db):.3f}')
    except Exception as e:
        print(f'  {name}: ERROR reading results ({e})')
"
echo ""
echo "Next: Deploy best model to devices and run on-device A/B test"
