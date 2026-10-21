# Blinky Time - Simplified Build System
# Cross-platform Arduino build automation
#
# Usage:
#   make compile                    # Compile (pinned toolchain; scripts/build.sh is preferred)
#   make monitor PORT=COM3          # Open serial monitor
#   make clean                      # Clean build artifacts
#   make list-boards                # List connected boards
#
# Flashing is NOT done from here (#270). Every flash must run a hardened path
# (CLAUDE.md "Upload Safety"):
#   fleet devices         ./scripts/deploy.sh --devices=...   (or POST /api/fleet/deploy)
#   a device on this USB  python tools/deploy_local.py --port ... --device-id ...
#   bootloader            ./scripts/deploy-bootloader.sh      (sacrificial device first)
# The old upload / flash / uf2-upload-all / bootloader-update targets refuse
# with that pointer. The one exception, per CLAUDE.md, is bare-chip recovery
# of a bricked device that isn't enrolled yet:
#   make uf2-upload UPLOAD_PORT=/dev/ttyACM0   # bare-chip recovery ONLY
#   make uf2-check UPLOAD_PORT=/dev/ttyACM0    # compile + validate (dry run)
#   make uf2-test                               # verify upload infrastructure
#
# Author: Blinky Time Project Contributors

# Configuration
SKETCH_DIR = blinky-things
FQBN = Seeeduino:nrf52:xiaonRF52840Sense
# USBMode=hwcdc required for serial on ESP32 core 3.3.7+ (core bug: TinyUSB mode
# has unresolved HWCDCSerial refs in chip-debug-report.cpp). This enables the
# USB_SERIAL_JTAG peripheral which shares GPIO42/41 with the PDM mic — see
# Esp32PdmMic::begin() for the gpio_reset_pin() workaround.
ESP32_FQBN = esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240
BAUD_RATE = 115200

# Default values
PORT ?= COM3

# UF2 upload settings (Linux)
UPLOAD_PORT ?= /dev/ttyACM0
BUILD_OUTPUT_DIR ?= /tmp/blinky-build
ESP32_BUILD_OUTPUT_DIR ?= /tmp/blinky-esp32-build
UF2_UPLOAD_TOOL = tools/uf2_upload.py

# Arduino CLI detection — must be on PATH
ifeq ($(OS),Windows_NT)
    ARDUINO_CLI = arduino-cli.exe
else
    ARDUINO_CLI = arduino-cli
endif

# Flashing targets that skip a hardened path refuse, and say which one to use
# (#270). Upload safety is the mechanisms, not a tool's name (CLAUDE.md):
# stable-only or test-device-only firmware, identity and bootloader checks,
# the single flash path, version verify, settings restore + save.
define REFUSE_FLASH
@echo "REFUSED: make does not flash devices (CLAUDE.md 'Upload Safety', issue 270)." >&2
@echo "  fleet:              ./scripts/deploy.sh --devices=<ids|all>   (or POST /api/fleet/deploy)" >&2
@echo "  device on this USB: python tools/deploy_local.py --port <port> --device-id <id> [--dev]" >&2
@echo "  bootloader:         ./scripts/deploy-bootloader.sh  (sacrificial device first)" >&2
@exit 1
endef

# Default target
.PHONY: help
help:
	@echo ""
	@echo "========================================"
	@echo "Blinky Time Build System"
	@echo "========================================"
	@echo ""
	@echo "Available targets:"
	@echo "  compile      - Compile sketch only (scripts/build.sh is preferred)"
	@echo "  monitor      - Open serial monitor"
	@echo "  clean        - Clean build artifacts"
	@echo "  list-boards  - List connected Arduino boards"
	@echo "  version      - Regenerate Version.h from BUILD_NUMBER"
	@echo ""
	@echo "Flashing: not from make. Use scripts/deploy.sh (fleet), tools/deploy_local.py"
	@echo "(a device on this USB) or scripts/deploy-bootloader.sh (bootloader)."
	@echo ""
	@echo "Bare-chip recovery only (a bricked device not yet enrolled):"
	@echo "  uf2-upload       - Compile + UF2 upload to one nRF52840 (UPLOAD_PORT)"
	@echo "  uf2-check        - Compile + validate + convert (dry run)"
	@echo "  uf2-test         - Verify upload infrastructure is ready"
	@echo "  safety-check     - Run pre-compile safety checks only"
	@echo ""
	@echo "ESP32-S3 (next-gen; not on the pinned toolchain):"
	@echo "  esp32-compile    - Compile for XIAO ESP32-S3"
	@echo "  esp32-uf2-upload - Compile + upload to ESP32-S3 via UF2"
	@echo "  esp32-uf2-check  - Compile + validate UF2 (dry run)"
	@echo ""
	@echo "Setup:"
	@echo "  setup-toolchain  - Pinned, isolated firmware toolchain in .arduino/ (once per machine)"
	@echo "  setup-tflite     - Install TFLite Micro Arduino library"
	@echo ""
	@echo "Parameters:"
	@echo "  PORT         - Serial port for monitor (default: COM3)"
	@echo "  UPLOAD_PORT  - Serial port for bare-chip uf2-upload (default: /dev/ttyACM0)"
	@echo ""
	@echo "Examples:"
	@echo "  make compile"
	@echo "  make uf2-upload UPLOAD_PORT=/dev/ttyACM0   # bare-chip recovery only"
	@echo "  make monitor PORT=/dev/ttyACM0"

# Check if Arduino CLI is installed
.PHONY: check-arduino-cli
check-arduino-cli:
	@echo "Checking Arduino CLI..."
	@$(ARDUINO_CLI) version || (echo "Arduino CLI not found! Please install it." && exit 1)
	@echo "Arduino CLI found"

# Compile sketch
.PHONY: compile
compile: check-arduino-cli version
	@echo ""
	@echo "========================================"
	@echo "Compiling"
	@echo "========================================"
	@echo "Sketch: $(SKETCH_DIR)"
	@echo "Board: $(FQBN)"
	@cd $(SKETCH_DIR) && bash ../scripts/in_toolchain.sh $(ARDUINO_CLI) compile --fqbn $(FQBN) $(if $(EXTRA_CPP_FLAGS),--build-property "compiler.cpp.extra_flags=$(EXTRA_CPP_FLAGS)") .
	@echo "Compilation successful!"

# Upload sketch: arduino-cli upload uses the DFU serial protocol, whose race
# conditions can brick an nRF52840 (SWD needed to recover). Refused.
.PHONY: upload
upload:
	$(REFUSE_FLASH)

# Open serial monitor
.PHONY: monitor
monitor: check-arduino-cli
	@echo ""
	@echo "========================================"
	@echo "Serial Monitor"
	@echo "========================================"
	@echo "Port: $(PORT), Baud: $(BAUD_RATE)"
	@echo "Press Ctrl+C to exit"
	@echo ""
	@$(ARDUINO_CLI) monitor --port $(PORT) --config baudrate=$(BAUD_RATE)

# List connected boards
.PHONY: list-boards
list-boards: check-arduino-cli
	@echo ""
	@echo "========================================"
	@echo "Connected Arduino Boards"
	@echo "========================================"
	@$(ARDUINO_CLI) board list

# Clean build artifacts
.PHONY: clean
clean:
	@echo ""
	@echo "========================================"
	@echo "Cleaning Build Artifacts"
	@echo "========================================"
ifeq ($(OS),Windows_NT)
	@cd $(SKETCH_DIR) && if exist build rmdir /s /q build
	@cd $(SKETCH_DIR) && del /q *.hex *.bin *.elf 2>nul || echo ""
else
	@cd $(SKETCH_DIR) && rm -rf build/ *.hex *.bin *.elf 2>/dev/null || true
endif
	@echo "Build artifacts cleaned"

# Regenerate Version.h from BUILD_NUMBER (no bump; scripts/build.sh bumps).
# Same generator as build.sh, so the git SHA is embedded either way (#213).
.PHONY: version
version:
	@bash scripts/gen_version.sh "$$(cat $(SKETCH_DIR)/BUILD_NUMBER | tr -d '[:space:]')"

# --- UF2 (bare-chip recovery only) ---

# Pre-compile safety checks (blocks build on critical failures)
.PHONY: safety-check
safety-check:
	@python3 scripts/pre_compile_safety.py

# Compile to external output directory
.PHONY: compile-out
compile-out: safety-check check-arduino-cli version
	@echo ""
	@echo "========================================"
	@echo "Compiling to $(BUILD_OUTPUT_DIR)"
	@echo "========================================"
	@echo "Sketch: $(SKETCH_DIR)"
	@echo "Board: $(FQBN)"
	@bash scripts/in_toolchain.sh $(ARDUINO_CLI) compile --fqbn $(FQBN) $(if $(EXTRA_CPP_FLAGS),--build-property "compiler.cpp.extra_flags=$(EXTRA_CPP_FLAGS)") --output-dir $(BUILD_OUTPUT_DIR) $(SKETCH_DIR)/
	@echo "Compilation successful!"

# UF2 upload: compile + validate + convert + upload. BARE-CHIP RECOVERY ONLY
# (CLAUDE.md): a bricked device that isn't enrolled yet. It flashes a local
# build with none of deploy_local.py's gates, so never use it on a configured
# device.
.PHONY: uf2-upload
uf2-upload: compile-out
	@echo ""
	@echo "========================================"
	@echo "UF2 Upload to $(UPLOAD_PORT) (bare-chip recovery)"
	@echo "========================================"
	python3 $(UF2_UPLOAD_TOOL) $(UPLOAD_PORT) --build-dir $(BUILD_OUTPUT_DIR) $(if $(VERBOSE),--verbose)

# A local build to every connected device: skips the test-device rule,
# identity checks and post-flash checks. Refused.
.PHONY: uf2-upload-all
uf2-upload-all:
	$(REFUSE_FLASH)

# UF2 dry run: compile + validate + convert only
.PHONY: uf2-check
uf2-check: compile-out
	python3 $(UF2_UPLOAD_TOOL) $(UPLOAD_PORT) --build-dir $(BUILD_OUTPUT_DIR) --dry-run

# Upload self-test: verify infrastructure
.PHONY: uf2-test
uf2-test:
	python3 $(UF2_UPLOAD_TOOL) --self-test

# --- Bootloader: only through scripts/deploy-bootloader.sh ---
# Its three safety layers (source invariant check, pre-flash version log,
# post-flash verify) exist because of the 2026-05-15 hat brick. Refused here.
.PHONY: bootloader-update bootloader-update-all
bootloader-update bootloader-update-all:
	$(REFUSE_FLASH)

# Pinned, isolated firmware toolchain in .arduino/ (#252): run once per machine.
.PHONY: setup-toolchain
setup-toolchain:
	@bash scripts/setup_toolchain.sh

# Install TFLite Micro Arduino library (required for NN=1 builds)
.PHONY: setup-tflite
setup-tflite:
	@echo "Installing TFLite Micro Arduino library..."
	@bash tools/setup_tflite_lib.sh

# --- ESP32-S3 targets (XIAO ESP32-S3 / Display board) ---

# Compile for ESP32-S3
.PHONY: esp32-compile
esp32-compile: check-arduino-cli version
	@echo ""
	@echo "========================================"
	@echo "Compiling for XIAO ESP32-S3"
	@echo "========================================"
	@echo "Sketch: $(SKETCH_DIR)"
	@echo "Board:  $(ESP32_FQBN)"
	@$(ARDUINO_CLI) compile --fqbn $(ESP32_FQBN) --output-dir $(ESP32_BUILD_OUTPUT_DIR) $(SKETCH_DIR)/
	@echo "Compilation successful!"
	@echo "UF2 output: $(ESP32_BUILD_OUTPUT_DIR)/blinky-things.ino.uf2"

# UF2 upload to ESP32-S3: compile + upload via UF2 mass-storage bootloader
.PHONY: esp32-uf2-upload
esp32-uf2-upload: esp32-compile
	@echo ""
	@echo "========================================"
	@echo "UF2 Upload to ESP32-S3 on $(UPLOAD_PORT)"
	@echo "========================================"
	python3 $(UF2_UPLOAD_TOOL) $(UPLOAD_PORT) --board esp32s3 --build-dir $(ESP32_BUILD_OUTPUT_DIR) $(if $(VERBOSE),--verbose)

# Dry run: compile + validate only (no upload)
.PHONY: esp32-uf2-check
esp32-uf2-check: esp32-compile
	python3 $(UF2_UPLOAD_TOOL) $(UPLOAD_PORT) --board esp32s3 --build-dir $(ESP32_BUILD_OUTPUT_DIR) --dry-run

# Quick aliases (flash is refused, like upload)
.PHONY: build flash serial boards
build: compile
flash: upload
serial: monitor
boards: list-boards

# Label reviewer — launches the multi-corpus label review web UI.
# Defaults register the training + edm corpora; switch in the UI dropdown.
.PHONY: reviewer
reviewer:
	$(MAKE) -C ml-training reviewer
