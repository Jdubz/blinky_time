#!/bin/bash
set -e
cd /home/jdubz/Development/blinky_time/ml-training
source venv/bin/activate

echo "=== V17 DATA PREPARATION ==="
PYTHONUNBUFFERED=1 python scripts/prepare_dataset.py \
  --config configs/conv1d_w16_onset_v17.yaml \
  --augment --band-flux \
  --teacher-soft-dir /mnt/storage/blinky-ml-data/labels/onset_teacher_soft \
  --output-dir data/processed_v17

echo "=== V17 TRAINING ==="
PYTHONUNBUFFERED=1 python train.py \
  --config configs/conv1d_w16_onset_v17.yaml \
  --data-dir data/processed_v17 \
  --output-dir outputs/v17-band-flux \
  --distill data/processed_v17/Y_teacher_train.npy

echo "=== V17 COMPLETE ==="
