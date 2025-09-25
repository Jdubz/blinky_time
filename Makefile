# Blinky Time - Simplified Build System
# Cross-platform Arduino build automation
#
# Usage:
#   make upload DEVICE=2            # Upload device type 2 (Tube Light) 
#   make compile DEVICE=1           # Compile device type 1 (Hat)
#   make monitor PORT=COM3          # Open serial monitor
#   make clean                      # Clean build artifacts
#   make list-boards                # List connected boards
#
# Author: Blinky Time Project Contributors

# Configuration
SKETCH_DIR = blinky-things
FQBN = Seeeduino:nrf52:xiaonRF52840Sense
BAUD_RATE = 115200

# Default values
DEVICE ?= 2
PORT ?= COM3

# Arduino CLI detection
ifeq ($(OS),Windows_NT)
    ARDUINO_CLI = arduino-cli.exe
else
    ARDUINO_CLI = arduino-cli
endif

# Default target
.PHONY: help
help:
	@echo ""
	@echo "========================================"
	@echo "Blinky Time Build System"
	@echo "========================================"
	@echo ""
	@echo "Available targets:"
	@echo "  upload       - Compile and upload sketch"
	@echo "  compile      - Compile sketch only" 
	@echo "  monitor      - Open serial monitor"
	@echo "  clean        - Clean build artifacts"
	@echo "  list-boards  - List connected Arduino boards"
	@echo "  version      - Update version from VERSION file"
	@echo ""
	@echo "Parameters:"
	@echo "  DEVICE       - Device type (1=Hat, 2=TubeLight, 3=BucketTotem)"
	@echo "  PORT         - Serial port (default: COM3)"
	@echo ""
	@echo "Examples:"
	@echo "  make upload DEVICE=2"
	@echo "  make upload DEVICE=1 PORT=COM4"
	@echo "  make compile DEVICE=3"
	@echo "  make monitor PORT=COM3"

# Check if Arduino CLI is installed
.PHONY: check-arduino-cli
check-arduino-cli:
	@echo "Checking Arduino CLI..."
	@$(ARDUINO_CLI) version || (echo "Arduino CLI not found! Please install it." && exit 1)
	@echo "Arduino CLI found"

# Update device type in sketch
.PHONY: update-device-type
update-device-type:
	@echo "Setting device type to $(DEVICE)"
ifeq ($(OS),Windows_NT)
	@powershell -Command "(Get-Content '$(SKETCH_DIR)/blinky-things.ino') -replace '#define DEVICE_TYPE [0-9]+', '#define DEVICE_TYPE $(DEVICE)' | Set-Content '$(SKETCH_DIR)/blinky-things.ino'"
else
	@sed -i 's/#define DEVICE_TYPE [0-9]\+/#define DEVICE_TYPE $(DEVICE)/' $(SKETCH_DIR)/blinky-things.ino
endif
	@echo "Device type updated to $(DEVICE)"

# Compile sketch
.PHONY: compile
compile: check-arduino-cli update-device-type version
	@echo ""
	@echo "========================================"
	@echo "Compiling Device Type $(DEVICE)"
	@echo "========================================"
	@echo "Sketch: $(SKETCH_DIR)"
	@echo "Board: $(FQBN)"
	@cd $(SKETCH_DIR) && $(ARDUINO_CLI) compile --fqbn $(FQBN) .
	@echo "Compilation successful!"

# Upload sketch
.PHONY: upload
upload: compile
	@echo ""
	@echo "========================================"
	@echo "Uploading to $(PORT)"
	@echo "========================================"
	@echo "This may take 10-30 seconds..."
	@cd $(SKETCH_DIR) && $(ARDUINO_CLI) upload --fqbn $(FQBN) --port $(PORT) .
	@echo ""
	@echo "========================================"
	@echo "Upload Complete!"
	@echo "========================================"
	@echo "Successfully uploaded Device Type $(DEVICE) to $(PORT)"
	@echo ""
	@echo "To view serial output:"
	@echo "   make monitor PORT=$(PORT)"

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

# Update version information from VERSION file
.PHONY: version
version:
	@echo "Updating version information..."
ifeq ($(OS),Windows_NT)
	@powershell -File scripts/update-version.ps1
else
	@VERSION=$$(cat VERSION | tr -d '\n'); \
	MAJOR=$$(echo $$VERSION | cut -d. -f1); \
	MINOR=$$(echo $$VERSION | cut -d. -f2); \
	PATCH=$$(echo $$VERSION | cut -d. -f3); \
	sed -i "s/#define BLINKY_VERSION_MAJOR [0-9]\+/#define BLINKY_VERSION_MAJOR $$MAJOR/" $(SKETCH_DIR)/core/Version.h; \
	sed -i "s/#define BLINKY_VERSION_MINOR [0-9]\+/#define BLINKY_VERSION_MINOR $$MINOR/" $(SKETCH_DIR)/core/Version.h; \
	sed -i "s/#define BLINKY_VERSION_PATCH [0-9]\+/#define BLINKY_VERSION_PATCH $$PATCH/" $(SKETCH_DIR)/core/Version.h; \
	sed -i "s/#define BLINKY_VERSION_STRING \"[^\"]*\"/#define BLINKY_VERSION_STRING \"$$VERSION\"/" $(SKETCH_DIR)/core/Version.h
endif
	@echo "Version updated to $$(cat VERSION)"

# Quick aliases
.PHONY: build flash serial boards
build: compile
flash: upload
serial: monitor
boards: list-boards