# ML Training Pipeline — Makefile
#
# Atomic targets that can be chained into full pipelines.
# Each target is idempotent (skips work already done via --overwrite=false).
#
# Usage:
#   make all                  # Full pipeline: download → label → merge → prepare → train → export → eval
#   make label                # Just labeling (all systems, parallel)
#   make train                # Just training (assumes data is prepared)
#   make prepare              # Feature extraction + augmentation
#   make export               # TFLite INT8 export
#
#   make label SYSTEMS=madmom WORKERS=5   # Label with specific system
#   make prepare AUGMENT=1                # Enable augmentation
#
# Background pipeline:
#   nohup make all > /mnt/storage/blinky-ml-data/pipeline.log 2>&1 &

SHELL := /bin/bash

# --- Paths ---
STORAGE       := /mnt/storage/blinky-ml-data
AUDIO_DIR     := $(STORAGE)/audio/combined
LABELS_MULTI  := $(STORAGE)/labels/multi
LABELS_CONSENSUS := $(STORAGE)/labels/consensus
PROCESSED_DIR := data/processed
OUTPUT_DIR    := outputs/full-v1
RIR_DIR       := $(STORAGE)/rir/processed
CONFIG        := configs/default.yaml
TEST_AUDIO    := ../blinky-test-player/music/edm

# --- Tunables ---
SYSTEMS       := beat_this,essentia,librosa,madmom
WORKERS       := 5
THREADS       := 4
DEVICE        := cuda
EPOCHS        := 100
AUGMENT       := 1
MIN_AGREEMENT := 2
TOLERANCE     := 0.05

# --- Derived ---
AUGMENT_FLAG  := $(if $(filter 1 yes true,$(AUGMENT)),--augment,)
PYTHON        := python3

# ============================================================
# Phony targets
# ============================================================
.PHONY: all download label merge prepare train export eval clean help

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
	@# Test tracks
	-ln -sf $(CURDIR)/../blinky-test-player/music/edm/*.mp3 $(AUDIO_DIR)/
	-ln -sf $(STORAGE)/labels/edm-test/*.beats.json $(STORAGE)/labels/combined/
	@# GiantSteps
	-ln -sf $(STORAGE)/audio/giantsteps/giantsteps-tempo-dataset/audio/*.mp3 $(AUDIO_DIR)/
	-ln -sf $(STORAGE)/labels/giantsteps/*.beats.json $(STORAGE)/labels/combined/
	@# FMA
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
		$(AUGMENT_FLAG)

# ============================================================
# Step 6: Train model
# ============================================================
train:
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
		--output-dir $(OUTPUT_DIR)

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
.PHONY: validate-features status

validate-features:
	$(PYTHON) scripts/validate_features.py --generate-reference \
		--audio $(TEST_AUDIO)/$$(ls $(TEST_AUDIO)/*.mp3 | head -1 | xargs basename) \
		--config $(CONFIG)

status:
	@echo "=== Dataset Status ==="
	@echo "Audio files:      $$(ls $(AUDIO_DIR)/*.mp3 2>/dev/null | wc -l)"
	@echo "beat_this labels:  $$(ls $(LABELS_MULTI)/*.beat_this.beats.json 2>/dev/null | wc -l)"
	@echo "essentia labels:   $$(ls $(LABELS_MULTI)/*.essentia.beats.json 2>/dev/null | wc -l)"
	@echo "librosa labels:    $$(ls $(LABELS_MULTI)/*.librosa.beats.json 2>/dev/null | wc -l)"
	@echo "madmom labels:     $$(ls $(LABELS_MULTI)/*.madmom.beats.json 2>/dev/null | wc -l)"
	@echo "consensus labels:  $$(ls $(LABELS_CONSENSUS)/*.beats.json 2>/dev/null | wc -l)"
	@echo "processed train:   $$([ -f $(PROCESSED_DIR)/X_train.npy ] && python3 -c "import numpy; print(len(numpy.load('$(PROCESSED_DIR)/X_train.npy', mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "processed val:     $$([ -f $(PROCESSED_DIR)/X_val.npy ] && python3 -c "import numpy; print(len(numpy.load('$(PROCESSED_DIR)/X_val.npy', mmap_mode='r')))" 2>/dev/null || echo 'none')"
	@echo "best model:        $$([ -f $(OUTPUT_DIR)/best_model.pt ] && echo 'yes' || echo 'no')"

clean:
	@echo "This would delete processed data and outputs. Use 'make clean-confirm' to proceed."

clean-confirm:
	rm -rf $(PROCESSED_DIR)/*.npy
	rm -rf $(OUTPUT_DIR)

help:
	@echo "ML Training Pipeline"
	@echo ""
	@echo "Targets:"
	@echo "  all       Full pipeline (download → label → merge → prepare → train → export → eval)"
	@echo "  download  Download FMA, GiantSteps, and RIR datasets"
	@echo "  label     Multi-system beat labeling (parallel CPU workers)"
	@echo "  merge     Merge per-system labels into consensus"
	@echo "  prepare   Extract mel features + augmentation"
	@echo "  train     Train beat activation CNN"
	@echo "  export    Export TFLite INT8 model"
	@echo "  eval      Evaluate on test tracks"
	@echo "  status    Show dataset/pipeline status"
	@echo ""
	@echo "Options (override with make target VAR=value):"
	@echo "  SYSTEMS=$(SYSTEMS)"
	@echo "  WORKERS=$(WORKERS)  THREADS=$(THREADS)"
	@echo "  DEVICE=$(DEVICE)  EPOCHS=$(EPOCHS)"
	@echo "  AUGMENT=$(AUGMENT)  MIN_AGREEMENT=$(MIN_AGREEMENT)"
