#!/usr/bin/env bash
# v34d post-train pipeline: export + eval + side-by-side comparison vs v33.
#
# Run after v34d training stops (early-stop on patience or epoch 60).
# Produces:
#   /mnt/storage/blinky-ml-data/outputs/v34d/{export.log, eval/}
#   /mnt/storage/blinky-ml-data/outputs/v34d/comparison_v33_v34d.txt
#
# All steps fail loudly. No silent skips. If anything goes wrong the
# script halts and surfaces the error rather than producing a "completed"
# log on a half-baked pipeline.

set -euo pipefail

# Resolve ROOT relative to this script so the pipeline works on any checkout.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V34D_OUT="/mnt/storage/blinky-ml-data/outputs/v34d"
# v33's training artifacts predate the move to /mnt/storage and live under
# the in-repo outputs/ tree. We invoke evaluate.py directly (rather than via
# `make eval`) so we can point at the right OUTPUT_DIR for v33.
V33_OUT="${ROOT}/outputs/v33_mel_only"
V34D_CFG="configs/conv1d_w16_onset_v34d_clean_labels.yaml"
V33_CFG="configs/conv1d_w16_onset_v33_mel_only.yaml"
TEST_AUDIO="../blinky-test-player/music/edm"

echo "=== v34d post-train pipeline ==="
echo "v34d output dir: ${V34D_OUT}"
echo "v33 output dir:  ${V33_OUT}"
echo

# Halt loudly if training didn't actually produce a model
[ -f "${V34D_OUT}/best_model.pt" ] || { echo "ERROR: v34d best_model.pt missing — training did not complete"; exit 1; }
[ -f "${V34D_OUT}/training_checkpoint.pt" ] || { echo "ERROR: v34d training_checkpoint.pt missing — training was killed mid-step"; exit 1; }

cd "${ROOT}"
source venv/bin/activate

# ---------- Step 1: export v34d to TFLite ----------
echo ">>> Export v34d to TFLite INT8"
make export \
    CONFIG="${V34D_CFG}" \
    RUN_NAME=v34d \
    | tee "${V34D_OUT}/export.log"

[ -f "${V34D_OUT}/frame_onset_model_data_int8.tflite" ] || \
    { echo "ERROR: v34d export produced no .tflite"; exit 1; }

# Surface tflite size — should be ≤27 KB per config
TFLITE_KB=$(du -k "${V34D_OUT}/frame_onset_model_data_int8.tflite" | cut -f1)
echo "v34d tflite size: ${TFLITE_KB} KB (config max: 27 KB)"
[ "${TFLITE_KB}" -le 27 ] || \
    { echo "ERROR: v34d tflite ${TFLITE_KB} KB > 27 KB budget"; exit 1; }

# ---------- Step 2: offline eval on edm/ — v34d ----------
echo
echo ">>> Offline eval v34d on edm/"
make eval \
    CONFIG="${V34D_CFG}" \
    RUN_NAME=v34d \
    | tee "${V34D_OUT}/eval.log"

# ---------- Step 3: offline eval on edm/ — v33 (apples-to-apples) ----------
# v33 was previously evaluated on-device (b153 = 0.570). Rerun the OFFLINE
# eval here so v33 + v34d are scored under the same code path, against the
# same hand-curated onsets_consensus GT, with the post-2026-04-29 evaluator
# fix that requires onset GT (not the legacy beat GT). Invoking evaluate.py
# directly (not `make eval`) because v33's artifacts live under the in-repo
# outputs/ tree, not /mnt/storage.
[ -f "${V33_OUT}/model_checkpoint.pt" ] || \
    { echo "ERROR: v33 model_checkpoint.pt not found at ${V33_OUT}"; exit 1; }
echo
echo ">>> Offline eval v33 on edm/ (re-baseline under onset-GT evaluator)"
mkdir -p "${V33_OUT}/eval"
# Use ${ROOT}/evaluate.py rather than the bare relative path: the script
# does cd "${ROOT}" at top, but defensive coding here means a future
# refactor that moves the cd out (or runs the script in a subshell)
# won't silently fail to find evaluate.py.
# Log is placed inside eval/ alongside the JSON artifacts for consistency
# (the JSON goes to "${V33_OUT}/eval/eval_results.json" via --output-dir).
python3 "${ROOT}/evaluate.py" \
    --config "${V33_CFG}" \
    --model "${V33_OUT}/model_checkpoint.pt" \
    --audio-dir "${TEST_AUDIO}" \
    --output-dir "${V33_OUT}/eval" \
    | tee "${V33_OUT}/eval/eval_2026_04_29.log"

# ---------- Step 4: side-by-side comparison ----------
echo
echo ">>> Side-by-side comparison"
python3 scripts/v34d_compare.py \
    "${V33_OUT}/eval/eval_results.json" \
    "${V34D_OUT}/eval/eval_results.json" \
    | tee "${V34D_OUT}/comparison_v33_v34d.txt"

echo
echo "=== Done. Comparison: ${V34D_OUT}/comparison_v33_v34d.txt ==="
