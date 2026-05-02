#!/usr/bin/env bash
# v36-fmax post-train pipeline: export + offline eval + side-by-side
# comparison vs v34d (apples-to-apples on clean labels) and vs v33
# (deployment-baseline edm/ Onset F1).
#
# v36 changed four audio dimensions at once vs v34d (sr 16k→31.25k,
# n_fft 256→512, n_mels 50→80, fmax 8k→14k) on top of the kick_weighted
# clean-labels switch v34d already adopted. The right baseline question
# for "did the audio change help?" is v36 vs v34d on the same clean
# labels. The right baseline question for "is v36 deployment-ready?" is
# v36 vs v33 on the actual edm/ corpus.
#
# Produces:
#   /mnt/storage/blinky-ml-data/outputs/v36_fmax/{export.log, eval/}
#   /mnt/storage/blinky-ml-data/outputs/v36_fmax/comparison_v34d_v36.txt
#   /mnt/storage/blinky-ml-data/outputs/v36_fmax/comparison_v33_v36.txt
#
# What this script does NOT do:
#   - Compile firmware. v36 changes SpectralConstants (NUM_MEL_BANDS
#     30→80, MEL_MAX_FREQ 8000→14000, FFT_SIZE 256→512, SAMPLE_RATE
#     16000→31250). The MEL_BANDS table must be regenerated via
#     scripts/generate_mel_bands.py and pasted into
#     SharedSpectralAnalysis.cpp before firmware will compile. That's a
#     manual rev, not script-safe.
#   - On-device validation. Requires the firmware rev above + a fleet
#     deploy via scripts/deploy.sh.
#
# Both deferred steps are documented below; run them after this script's
# offline numbers justify spending the firmware time.
#
# All steps fail loudly. No silent skips.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
V36_OUT="/mnt/storage/blinky-ml-data/outputs/v36_fmax"
V34D_OUT="/mnt/storage/blinky-ml-data/outputs/v34d"
# v33 artifacts predate the move to /mnt/storage and live under the in-repo
# outputs/ tree. Already evaluated in eval_2026_04_29.log.
V33_OUT="${ROOT}/outputs/v33_mel_only"
V36_CFG="configs/conv1d_w16_onset_v36_fmax.yaml"
TEST_AUDIO="../blinky-test-player/music/edm"

echo "=== v36-fmax post-train pipeline ==="
echo "v36 output dir:  ${V36_OUT}"
echo "v34d output dir: ${V34D_OUT}"
echo "v33 output dir:  ${V33_OUT}"
echo

[ -f "${V36_OUT}/best_model.pt" ] || { echo "ERROR: v36 best_model.pt missing — training did not complete"; exit 1; }
[ -f "${V36_OUT}/training_checkpoint.pt" ] || { echo "ERROR: v36 training_checkpoint.pt missing — training was killed mid-step"; exit 1; }
[ -f "${V34D_OUT}/eval/eval_results.json" ] || { echo "ERROR: v34d eval_results.json missing — re-run v34d_post_train.sh first"; exit 1; }
[ -f "${V33_OUT}/eval/eval_results.json" ] || { echo "ERROR: v33 eval_results.json missing — re-run v34d_post_train.sh (Step 3) first"; exit 1; }

cd "${ROOT}"
source venv/bin/activate

# ---------- Step 1: export v36 to TFLite ----------
# Pass PROCESSED_DIR explicitly: the Makefile defaults to
# $(STORAGE)/processed (a symlink/dir convention from earlier runs that
# no longer exists). v36's prepped data lives at processed_v36/ per the
# config's data.processed_dir, but the Makefile passes --data-dir to
# export.py, which takes the CLI arg over the config. Without this
# override, the calibration step fails with FileNotFoundError on
# X_train.npy.
echo ">>> Export v36 to TFLite INT8"
make export \
    CONFIG="${V36_CFG}" \
    RUN_NAME=v36_fmax \
    PROCESSED_DIR=/mnt/storage/blinky-ml-data/processed_v36 \
    | tee "${V36_OUT}/export.log"

[ -f "${V36_OUT}/frame_onset_model_data_int8.tflite" ] || \
    { echo "ERROR: v36 export produced no .tflite"; exit 1; }

TFLITE_KB=$(du -k "${V36_OUT}/frame_onset_model_data_int8.tflite" | cut -f1)
echo "v36 tflite size: ${TFLITE_KB} KB (config max: 35 KB)"
[ "${TFLITE_KB}" -le 35 ] || \
    { echo "ERROR: v36 tflite ${TFLITE_KB} KB > 35 KB budget"; exit 1; }

# ---------- Step 2: offline eval on edm/ — v36 ----------
echo
echo ">>> Offline eval v36 on edm/"
make eval \
    CONFIG="${V36_CFG}" \
    RUN_NAME=v36_fmax \
    | tee "${V36_OUT}/eval.log"

[ -f "${V36_OUT}/eval/eval_results.json" ] || \
    { echo "ERROR: v36 eval produced no eval_results.json"; exit 1; }

# ---------- Step 3: side-by-side v36 vs v34d (apples-to-apples) ----------
# Same clean labels, same edm/ GT, same evaluator. The difference between
# these two numbers isolates the audio-config change (sr/n_fft/n_mels/fmax)
# from the label-cleaning change.
echo
echo ">>> Comparison: v36 vs v34d (audio-config delta on clean labels)"
python3 scripts/v34d_compare.py \
    "${V34D_OUT}/eval/eval_results.json" \
    "${V36_OUT}/eval/eval_results.json" \
    | tee "${V36_OUT}/comparison_v34d_v36.txt"

# ---------- Step 4: side-by-side v36 vs v33 (deployment baseline) ----------
# Different training-label distributions, but both evaluated against the
# same edm/ GT under the same evaluator (post-2026-04-29 onset-GT fix).
# The right question for deployment is whether v36 beats v33 on the
# corpus the system actually has to perform on, regardless of which
# label set each was trained against.
echo
echo ">>> Comparison: v36 vs v33 (deployment baseline on edm/)"
python3 scripts/v34d_compare.py \
    "${V33_OUT}/eval/eval_results.json" \
    "${V36_OUT}/eval/eval_results.json" \
    | tee "${V36_OUT}/comparison_v33_v36.txt"

echo
echo "=== Done. ==="
echo "  v36 vs v34d:  ${V36_OUT}/comparison_v34d_v36.txt"
echo "  v36 vs v33:   ${V36_OUT}/comparison_v33_v36.txt"
echo
echo "Next steps if v36 beats v33 on edm/ Onset F1:"
echo "  1. Regenerate MEL_BANDS table:"
echo "     python3 scripts/generate_mel_bands.py \\"
echo "       --n-mels 80 --fmin 30 --fmax 14000 --n-fft 512 --sr 31250 \\"
echo "       --output ../blinky-things/audio/mel_bands_v36.cpp.fragment"
echo "  2. Update SpectralConstants in SharedSpectralAnalysis.h:"
echo "     NUM_MEL_BANDS 30→80, MEL_MAX_FREQ 8000→14000,"
echo "     FFT_SIZE 256→512, SAMPLE_RATE 16000→31250"
echo "  3. Replace MEL_BANDS[] in SharedSpectralAnalysis.cpp"
echo "  4. ./scripts/deploy.sh"
echo "  5. Run on-device validation harness against edm/"
