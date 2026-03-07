# ML Training Pipeline — Makefile
#
# Atomic targets that chain into a full pipeline. Each run gets a unique
# output directory based on RUN_NAME (default: config name + timestamp).
#
# Usage:
#   make prepare train export eval     # Standard pipeline
#   make all                           # Full pipeline (download → ... → eval)
#   make train CONFIG=configs/wider_rf.yaml  # Train with wider model
#   make eval RUN_NAME=v4-final        # Evaluate a specific run
#
# Background pipeline:
#   nohup make all > pipeline.log 2>&1 &

SHELL := /bin/bash

# --- Paths ---
STORAGE       := /mnt/storage/blinky-ml-data
AUDIO_DIR     := $(STORAGE)/audio/combined
LABELS_MULTI  := $(STORAGE)/labels/multi
LABELS_CONSENSUS := $(STORAGE)/labels/consensus
PROCESSED_DIR := data/processed
RIR_DIR       := $(STORAGE)/rir/processed
TEST_AUDIO    := ../blinky-test-player/music/edm
CONFIG        := configs/wider_rf.yaml

# Run naming: auto-generates from config name + timestamp if not specified.
# Override with: make train RUN_NAME=my-experiment
_CONFIG_NAME  := $(basename $(notdir $(CONFIG)))
_TIMESTAMP    := $(shell date +%Y%m%d-%H%M%S)
RUN_NAME      ?= $(_CONFIG_NAME)-$(_TIMESTAMP)
OUTPUT_DIR    := $(STORAGE)/outputs/$(RUN_NAME)

# --- Tunables ---
SYSTEMS       := beat_this,essentia,librosa,madmom
WORKERS       := 5
THREADS       := 4
DEVICE        := cuda
EPOCHS        := 100
AUGMENT       := 1
MIC_PROFILE   :=
MIN_AGREEMENT := 2
TOLERANCE     := 0.05
INFERENCE_FRAMES :=

# --- Derived ---
AUGMENT_FLAG  := $(if $(filter 1 yes true,$(AUGMENT)),--augment,)
MIC_PROFILE_FLAG := $(if $(MIC_PROFILE),--mic-profile $(MIC_PROFILE),)
INFERENCE_FRAMES_FLAG := $(if $(INFERENCE_FRAMES),--inference-frames $(INFERENCE_FRAMES),)
PYTHON        := python3

# ============================================================
# Phony targets
# ============================================================
.PHONY: all download label merge prepare train export eval clean status help

all: download label merge prepare train export eval
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
	@mkdir -p $(AUDIO_DIR) $(STORAGE)/labels/combined
	-ln -sf $(CURDIR)/../blinky-test-player/music/edm/*.mp3 $(AUDIO_DIR)/
	-ln -sf $(STORAGE)/labels/edm-test/*.beats.json $(STORAGE)/labels/combined/
	-ln -sf $(STORAGE)/audio/giantsteps/giantsteps-tempo-dataset/audio/*.mp3 $(AUDIO_DIR)/
	-ln -sf $(STORAGE)/labels/giantsteps/*.beats.json $(STORAGE)/labels/combined/
	@if [ -d "$(STORAGE)/audio/fma" ]; then \
		for f in $(STORAGE)/audio/fma/*.mp3; do \
			[ -f "$$f" ] && ln -sf "$$f" $(AUDIO_DIR)/$$(basename "$$f"); \
		done; \
		for f in $(STORAGE)/labels/fma/*.beats.json; do \
			[ -f "$$f" ] && ln -sf "$$f" $(STORAGE)/labels/combined/$$(basename "$$f"); \
		done; \
	fi
	@echo "Combined: $$(ls $(AUDIO_DIR)/*.mp3 2>/dev/null | wc -l) audio files"

# ============================================================
# Step 3: Label beats (parallel CPU, sequential GPU)
# ============================================================
label: combine-audio
	$(PYTHON) scripts/label_beats.py \
		--audio-dir $(AUDIO_DIR) \
		--output-dir $(LABELS_MULTI) \
		--systems $(SYSTEMS) \
		--device $(DEVICE) \
		--workers $(WORKERS) \
		--threads-per-worker $(THREADS)

# ============================================================
# Step 4: Merge consensus labels
# ============================================================
merge:
	$(PYTHON) scripts/merge_consensus_labels.py \
		--labels-dir $(LABELS_MULTI) \
		--output-dir $(LABELS_CONSENSUS) \
		--tolerance $(TOLERANCE) \
		--min-agreement $(MIN_AGREEMENT)

# ============================================================
# Step 5: Prepare dataset (feature extraction + augmentation)
# ============================================================
prepare:
	$(PYTHON) scripts/prepare_dataset.py \
		--config $(CONFIG) \
		--audio-dir $(AUDIO_DIR) \
		--labels-dir $(LABELS_CONSENSUS) \
		--output-dir $(PROCESSED_DIR) \
		--rir-dir $(RIR_DIR) \
		$(AUGMENT_FLAG) $(MIC_PROFILE_FLAG)

# ============================================================
# Step 6: Train model
# ============================================================
train:
	@echo "Run: $(RUN_NAME)"
	@echo "Config: $(CONFIG)"
	@echo "Output: $(OUTPUT_DIR)"
	$(PYTHON) train.py \
		--config $(CONFIG) \
		--data-dir $(PROCESSED_DIR) \
		--output-dir $(OUTPUT_DIR) \
		--epochs $(EPOCHS)

# ============================================================
# Step 7: Export TFLite INT8
# ============================================================
export:
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
	@echo "Consensus labels: $$(ls $(LABELS_CONSENSUS)/*.beats.json 2>/dev/null | wc -l)"
	@echo "Processed train:  $$([ -f $(PROCESSED_DIR)/X_train.npy ] && DATADIR='$(PROCESSED_DIR)' $(PYTHON) -c "import numpy, os; print(len(numpy.load(os.path.join(os.environ['DATADIR'], 'X_train.npy'), mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "Processed val:    $$([ -f $(PROCESSED_DIR)/X_val.npy ] && DATADIR='$(PROCESSED_DIR)' $(PYTHON) -c "import numpy, os; print(len(numpy.load(os.path.join(os.environ['DATADIR'], 'X_val.npy'), mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "=== Recent Runs ==="
	@ls -dt $(STORAGE)/outputs/*/ 2>/dev/null | head -5

clean:
	@echo "This would delete processed data. Use 'make clean-confirm' to proceed."

clean-confirm:
	rm -rf $(PROCESSED_DIR)/*.npy

help:
	@echo "ML Training Pipeline"
	@echo ""
	@echo "Targets:"
	@echo "  all              Full pipeline (download → label → merge → prepare → train → export → eval)"
	@echo "  prepare          Extract mel features + augmentation"
	@echo "  train            Train beat activation CNN"
	@echo "  export           Export TFLite INT8 model"
	@echo "  eval             Evaluate on test tracks"
	@echo "  status           Show dataset/pipeline status"
	@echo "  calibrate        Mic calibration pipeline"
	@echo ""
	@echo "Key Options:"
	@echo "  CONFIG=$(CONFIG)"
	@echo "  RUN_NAME=<name>  (default: auto-timestamped from config name)"
	@echo "  AUGMENT=$(AUGMENT)  MIC_PROFILE=$(MIC_PROFILE)"
	@echo "  EPOCHS=$(EPOCHS)  DEVICE=$(DEVICE)"
	@echo "  INFERENCE_FRAMES=64  (optional: reduce context for export)"
	@echo ""
	@echo "Examples:"
	@echo "  make prepare train export eval                    # Full run with defaults"
	@echo "  make train CONFIG=configs/default.yaml            # Train 3-layer baseline"
	@echo "  make export RUN_NAME=wider_rf-20260306-173448     # Re-export a previous run"
	@echo "  make eval RUN_NAME=wider_rf-20260306-173448       # Evaluate a previous run"
