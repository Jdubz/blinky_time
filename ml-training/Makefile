# ML Training Pipeline — Makefile
#
# Atomic targets that chain into a full pipeline. Each run gets a unique
# output directory based on RUN_NAME (default: config name + timestamp).
#
# Usage (CONFIG is required: an ONSET config, e.g. configs/conv1d_w16_onset_v33_mel_only.yaml):
#   make prepare train export eval CONFIG=configs/<onset config>.yaml
#   make all CONFIG=...                 # download → onset labels → prepare → train → export → eval
#   make eval CONFIG=... RUN_NAME=v4-final
# Long runs go in tmux (CLAUDE.md); ./train_pipeline.sh is the usual entry point.

SHELL := /bin/bash

# --- Paths ---
# Storage tiering (see docs/ML_STORAGE_LAYOUT.md):
#   STORAGE — /mnt/storage SATA SSD: warm tier for audio corpora, labels, RIR, noise.
#   NVME    — /mnt/nvme LVM-linear pool over 2× USB-NVMe (~462 GB): hot tier for
#             processed_v*, mel_cache, training outputs. Single mount point so
#             prep peak (~370 GB shards+final during merge) doesn't surprise us.
STORAGE       := /mnt/storage/blinky-ml-data
NVME          := /mnt/nvme
AUDIO_DIR     := $(STORAGE)/audio/combined
# Onset labels (the actual training target — see CLAUDE.md "Task is Onset Detection").
# Was previously $(STORAGE)/labels/consensus_v2 (BEAT consensus); switched 2026-04-29
# to onsets_consensus to stop silently injecting beat-detection bias into training.
# The historical consensus_v* dirs were archived to labels/_archive_beats_2026_04_29/.
LABELS_ONSETS := $(STORAGE)/labels/onsets_consensus
# Legacy alias preserved so any out-of-tree script that imports this Makefile
# variable doesn't crash. Points at the onset path; never write beat labels here.
LABELS_CONSENSUS := $(LABELS_ONSETS)
PROCESSED_DIR := $(NVME)/processed
RIR_DIR       := $(STORAGE)/rir/processed
TEST_AUDIO    := ../blinky-test-player/music/edm
# No default: the old default (configs/wider_rf.yaml) was a legacy BEAT CNN (#271).
CONFIG        ?=
REQUIRE_CONFIG = $(if $(CONFIG),,$(error CONFIG is required: pass CONFIG=configs/<onset config>.yaml (#271)))

# Run naming: auto-generates from config name + timestamp if not specified.
# Override with: make train RUN_NAME=my-experiment
_CONFIG_NAME  := $(basename $(notdir $(CONFIG)))
_TIMESTAMP    := $(shell date +%Y%m%d-%H%M%S)
RUN_NAME      ?= $(_CONFIG_NAME)-$(_TIMESTAMP)
OUTPUT_DIR    := $(NVME)/outputs/$(RUN_NAME)

# --- Tunables ---
WORKERS       := 5
DEVICE        := cuda
EPOCHS        := 100
AUGMENT       := 1
MIC_PROFILE   :=
INFERENCE_FRAMES :=

# --- Derived ---
AUGMENT_FLAG  := $(if $(filter 1 yes true,$(AUGMENT)),--augment,)
MIC_PROFILE_FLAG := $(if $(MIC_PROFILE),--mic-profile $(MIC_PROFILE),)
INFERENCE_FRAMES_FLAG := $(if $(INFERENCE_FRAMES),--inference-frames $(INFERENCE_FRAMES),)
PYTHON        := python3

# ============================================================
# Phony targets
# ============================================================
.PHONY: all download label merge merge-v1 prepare train export eval clean status help

all: download label prepare train export eval
	@echo "Pipeline complete. Results in $(OUTPUT_DIR)/"

# ============================================================
# Step 1: Download datasets
# ============================================================
download: download-fma download-giantsteps download-rir

.PHONY: download-fma download-giantsteps download-rir

download-fma:
	$(PYTHON) scripts/download_fma.py --output-dir $(STORAGE)/audio/fma

download-giantsteps:
	$(PYTHON) scripts/download_giantsteps.py --output-dir $(STORAGE)/audio/giantsteps

download-rir:
	$(PYTHON) scripts/download_rir.py --output-dir $(STORAGE)/rir

# ============================================================
# Step 2: Build combined audio directory (symlinks)
# ============================================================
.PHONY: combine-audio
combine-audio:
	@mkdir -p $(AUDIO_DIR)
	-ln -sf $(CURDIR)/../blinky-test-player/music/edm/*.mp3 $(AUDIO_DIR)/
	-ln -sf $(STORAGE)/audio/giantsteps/giantsteps-tempo-dataset/audio/*.mp3 $(AUDIO_DIR)/
	@if [ -d "$(STORAGE)/audio/fma" ]; then \
		for f in $(STORAGE)/audio/fma/*.mp3; do \
			[ -f "$$f" ] && ln -sf "$$f" $(AUDIO_DIR)/$$(basename "$$f"); \
		done; \
	fi
	@echo "Combined: $$(ls $(AUDIO_DIR)/*.mp3 2>/dev/null | wc -l) audio files"

# ============================================================
# Step 3: Onset labels (5-system onset consensus; resumes, skips done tracks)
# ============================================================
label: combine-audio
	$(PYTHON) scripts/generate_onset_consensus.py \
		--audio-dir $(AUDIO_DIR) \
		--output-dir $(LABELS_ONSETS) \
		--workers $(WORKERS)

# Beat-consensus merging is gone (#271): `merge` wrote BEAT labels into the
# onset label dir. The task is onset detection (CLAUDE.md); beat outputs are
# never training labels.
merge merge-v1:
	@echo "REFUSED: '$@' merged BEAT consensus labels, which must never become training labels (CLAUDE.md, #271)." >&2
	@echo "  Onset labels: make label   (scripts/generate_onset_consensus.py)" >&2
	@exit 1

# ============================================================
# Step 5: Prepare dataset (feature extraction + augmentation)
# ============================================================
prepare:
	$(REQUIRE_CONFIG)
	$(PYTHON) scripts/prepare_dataset.py \
		--config $(CONFIG) \
		--audio-dir $(AUDIO_DIR) \
		--labels-dir $(LABELS_ONSETS) \
		--output-dir $(PROCESSED_DIR) \
		--rir-dir $(RIR_DIR) \
		--exclude-dir $(TEST_AUDIO) \
		$(AUGMENT_FLAG) $(MIC_PROFILE_FLAG)

# ============================================================
# Step 6: Train model
# ============================================================
train:
	$(REQUIRE_CONFIG)
	@echo "Run: $(RUN_NAME)"
	@echo "Config: $(CONFIG)"
	@echo "Output: $(OUTPUT_DIR)"
	$(PYTHON) train.py \
		--config $(CONFIG) \
		--data-dir $(PROCESSED_DIR) \
		--output-dir $(OUTPUT_DIR) \
		--epochs $(EPOCHS)

# ============================================================
# Step 7: Export TFLite INT8 (writes the config's output_header)
# ============================================================
export:
	$(REQUIRE_CONFIG)
	$(PYTHON) scripts/export_tflite.py \
		--config $(CONFIG) \
		--model $(OUTPUT_DIR)/best_model.pt \
		--data-dir $(PROCESSED_DIR) \
		--output-dir $(OUTPUT_DIR) \
		$(INFERENCE_FRAMES_FLAG)

# ============================================================
# Step 8: Evaluate on test tracks
# ============================================================
eval:
	$(REQUIRE_CONFIG)
	$(PYTHON) evaluate.py \
		--config $(CONFIG) \
		--model $(OUTPUT_DIR)/model_checkpoint.pt \
		--audio-dir $(TEST_AUDIO) \
		--output-dir $(OUTPUT_DIR)/eval

# ============================================================
# Utilities
# ============================================================
.PHONY: validate-features calibrate calibrate-generate calibrate-capture calibrate-analyze

validate-features:
	$(REQUIRE_CONFIG)
	$(PYTHON) scripts/validate_features.py --generate-reference \
		--audio $(TEST_AUDIO)/$$(ls $(TEST_AUDIO)/*.mp3 | head -1 | xargs basename) \
		--config $(CONFIG)

calibrate-generate:
	$(PYTHON) scripts/calibrate_mic.py generate --output-dir data/calibration

calibrate-capture:
	@echo "Run on blinkyhost with speakers connected:"
	@echo "  python3 scripts/calibrate_mic.py capture-all --ports /dev/ttyACM0,/dev/ttyACM1,/dev/ttyACM2"

calibrate-analyze:
	$(PYTHON) scripts/calibrate_mic.py analyze \
		--captures data/calibration \
		--output data/calibration/mic_profile.npz

calibrate: calibrate-generate
	@echo "Next: run 'make calibrate-capture' on blinkyhost, then 'make calibrate-analyze'"

status:
	@echo "=== Dataset Status ==="
	@echo "Audio files:      $$(ls $(AUDIO_DIR)/*.mp3 2>/dev/null | wc -l)"
	@echo "Onset labels:     $$(ls $(LABELS_ONSETS)/*.onsets.json 2>/dev/null | wc -l)"
	@echo "Processed train:  $$([ -f $(PROCESSED_DIR)/X_train.npy ] && DATADIR='$(PROCESSED_DIR)' $(PYTHON) -c "import numpy, os; print(len(numpy.load(os.path.join(os.environ['DATADIR'], 'X_train.npy'), mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "Processed val:    $$([ -f $(PROCESSED_DIR)/X_val.npy ] && DATADIR='$(PROCESSED_DIR)' $(PYTHON) -c "import numpy, os; print(len(numpy.load(os.path.join(os.environ['DATADIR'], 'X_val.npy'), mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "=== Recent Runs ==="
	@# Show NVMe runs (current OUTPUT_DIR root) and any leftover STORAGE runs
	@# from the pre-NVMe-migration era. After the migration is complete and
	@# old runs are archived/deleted, the STORAGE line will go silent.
	@ls -dt $(NVME)/outputs/*/ $(STORAGE)/outputs/*/ 2>/dev/null | head -5

REVIEWER       := $(PYTHON) tools/label-reviewer/server.py
REVIEWER_PORT  := 8765

# Launch the label reviewer. The default corpora (training + edm) are
# registered automatically; switch between them in the UI dropdown — no
# restart needed. Pass --corpus name=audio,onset,human to add more.
#
# Edits land in:
#   training corpus -> /mnt/storage/.../labels/onsets_human/  (devtop-local)
#   edm corpus      -> blinky-test-player/music/edm/          (commits via git
#                                                              -> blinkyhost)
reviewer:
	$(REVIEWER) --port $(REVIEWER_PORT)

clean:
	@echo "This would delete processed data. Use 'make clean-confirm' to proceed."

clean-confirm:
	rm -rf $(PROCESSED_DIR)/*.npy

help:
	@echo "ML Training Pipeline (onset detection)"
	@echo ""
	@echo "Targets:"
	@echo "  all              Full pipeline (download → onset labels → prepare → train → export → eval)"
	@echo "  label            Generate onset consensus labels (resumes)"
	@echo "  prepare          Extract mel features + augmentation"
	@echo "  train            Train the onset CNN"
	@echo "  export           Export TFLite INT8 model (writes the config's output_header)"
	@echo "  eval             Evaluate on test tracks"
	@echo "  status           Show dataset/pipeline status"
	@echo "  calibrate        Mic calibration pipeline"
	@echo ""
	@echo "Label curation:"
	@echo "  reviewer         Launch label reviewer (training + edm corpora; switch in UI)"
	@echo ""
	@echo "Key Options:"
	@echo "  CONFIG=<onset config>  (required for prepare/train/export/eval)"
	@echo "  RUN_NAME=<name>  (default: auto-timestamped from config name)"
	@echo "  AUGMENT=$(AUGMENT)  MIC_PROFILE=$(MIC_PROFILE)"
	@echo "  EPOCHS=$(EPOCHS)  DEVICE=$(DEVICE)"
	@echo "  INFERENCE_FRAMES=64  (optional: reduce context for export)"
	@echo ""
	@echo "Examples:"
	@echo "  make prepare train export eval CONFIG=configs/conv1d_w16_onset_v33_mel_only.yaml"
	@echo "  make export CONFIG=... RUN_NAME=<run>   # Re-export a previous run"
	@echo "  make eval CONFIG=... RUN_NAME=<run>     # Evaluate a previous run"
