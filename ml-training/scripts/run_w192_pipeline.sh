#!/bin/bash
# W192 Training Pipeline — prepare best possible data, then train once.
#
# This script runs inside tmux. Each step checks for existing output
# before running (resumable). If any step fails, the pipeline stops.
#
# Usage:
#   tmux new-session -d -s pipeline "cd ml-training && bash scripts/run_w192_pipeline.sh 2>&1 | tee outputs/pipeline.log"
#
# Steps:
#   1. Batch Demucs separation (all 7000 tracks → stems for augmentation + allin1)
#   2. Complete allin1 labeling (using cached stems, batch mode on GPU)
#   3. Build consensus_v5 (7 systems: beat_this, madmom, essentia, librosa, demucs_beats, beatnet, allin1)
#   4. Prepare dataset (consensus_v5 labels + drum stem augmentation + all other augmentations)
#   5. Train W192 model
#   6. Export + evaluate

set -eo pipefail

cd "$(dirname "$0")/.."
source venv/bin/activate

OUTPUTS="outputs"
DATA_ROOT="${BLINKY_DATA_ROOT:-/mnt/storage/blinky-ml-data}"
STEMS_DIR="$DATA_ROOT/stems"
LABELS_DIR="$DATA_ROOT/labels/multi"
CONSENSUS_V5_DIR="$DATA_ROOT/labels/consensus_v5"
AUDIO_DIR="$DATA_ROOT/audio/combined"

# Disk space requirements (GB)
REQUIRED_STORAGE_GB=75   # ~70 GB for Demucs stems
REQUIRED_NVME_GB=160     # ~150 GB for processed training data

echo "============================================================"
echo "  W192 Training Pipeline"
echo "  Started: $(date)"
echo "============================================================"
echo ""

# ──────────────────────────────────────────────────────────────────
# Preflight checks
# ──────────────────────────────────────────────────────────────────
echo "[Preflight] Checking prerequisites..."

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

STORAGE_FREE_KB=$(df --output=avail "$DATA_ROOT" | tail -1)
NVMe_FREE_KB=$(df --output=avail "data/processed" | tail -1)
echo "  Storage free ($DATA_ROOT): $((STORAGE_FREE_KB / 1024 / 1024)) GB (need ~${REQUIRED_STORAGE_GB} GB for stems)"
echo "  NVMe free (data/processed): $((NVMe_FREE_KB / 1024 / 1024)) GB (need ~${REQUIRED_NVME_GB} GB for data)"

if [ "$STORAGE_FREE_KB" -lt $((REQUIRED_STORAGE_GB * 1024 * 1024)) ]; then
    echo "  WARNING: Less than ${REQUIRED_STORAGE_GB} GB free — stems may not fit"
fi
if [ "$NVMe_FREE_KB" -lt $((REQUIRED_NVME_GB * 1024 * 1024)) ]; then
    echo "  WARNING: Less than ${REQUIRED_NVME_GB} GB free — processed data may not fit"
fi
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 1: Batch Demucs separation
# ──────────────────────────────────────────────────────────────────
echo "[Step 1/6] Batch Demucs source separation"
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
# Step 2: Complete allin1 labeling (using cached stems)
# ──────────────────────────────────────────────────────────────────
echo "[Step 2/6] allin1 labeling"
echo "  Started: $(date)"
ALLIN1_DONE=$(find "$LABELS_DIR" -maxdepth 1 -name "*.allin1.beats.json" 2>/dev/null | wc -l)
echo "  allin1 labels: $ALLIN1_DONE / $TOTAL_AUDIO"
if [ "$ALLIN1_DONE" -ge "$TOTAL_AUDIO" ]; then
    echo "  SKIP: All tracks already labeled"
else
    # --demix-dir points to stems from Step 1 (allin1 auto-detects cached stems).
    # --allin1-device cuda for GPU NN inference (~1s vs 86s CPU).
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
# Step 3: Build consensus_v5 (7 systems)
# ──────────────────────────────────────────────────────────────────
echo "[Step 3/6] Build consensus_v5 labels"
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
# Step 4: Prepare dataset (consensus_v5 + drum stem augmentation)
# ──────────────────────────────────────────────────────────────────
echo "[Step 4/6] Prepare dataset (consensus_v5 + drum stems + augmentation)"
echo "  Started: $(date)"
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
    --rir-dir "$DATA_ROOT/rir/processed" \
    --stems-dir "$STEMS_DIR" \
    --stem-variants drums
echo "  DONE: Dataset prepared at $(date)"
echo ""

# ──────────────────────────────────────────────────────────────────
# Step 5: Train W192
# ──────────────────────────────────────────────────────────────────
echo "[Step 5/6] Train W192 (consensus_v5 + drum stems)"
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
# Step 6: Export + evaluate
# ──────────────────────────────────────────────────────────────────
echo "[Step 6/6] Export + evaluate"
if [ -f "$OUTPUTS/w192/export/frame_beat_model_data_int8.tflite" ]; then
    echo "  SKIP: TFLite model already exported"
else
    mkdir -p "$OUTPUTS/w192/export"
    python scripts/export_tflite.py \
        --config configs/frame_fc_w192.yaml \
        --model "$OUTPUTS/w192/best_model.pt" \
        --output-dir "$OUTPUTS/w192/export"
    echo "  Export complete"
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
    echo "  Evaluation complete"
fi

# Print results
echo ""
echo "============================================================"
echo "  Pipeline Complete: $(date)"
echo "============================================================"
echo ""
python3 -c "
import json
try:
    with open('$OUTPUTS/w192/eval/eval_results.json') as f:
        data = json.load(f)
    if data:
        f1s = [t['f1'] for t in data]
        db = [t.get('db_f1', 0.0) for t in data]
        print(f'  W192 Result: Beat F1={sum(f1s)/len(f1s):.3f}, DB F1={sum(db)/len(db):.3f} ({len(data)} tracks)')
    else:
        print('  W192 Result: No evaluation data found.')
except Exception as e:
    print(f'  W192 Result: ERROR reading results ({e})')
"
echo ""
echo "Next: Deploy model to devices and run on-device A/B test"
