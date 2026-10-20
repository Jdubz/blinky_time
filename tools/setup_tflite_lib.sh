#!/usr/bin/env bash
# Install TFLite Micro Arduino library for XIAO nRF52840 Sense builds.
#
# Uses the Seeed-maintained fork of tensorflow/tflite-micro-arduino-examples
# which is tested on the XIAO BLE Sense (our exact hardware).
#
# Patches applied automatically:
# - Fix board define case mismatch (ARDUINO_SEEED_XIAO_NRF52840_SENSE →
#   ARDUINO_Seeed_XIAO_nRF52840_Sense) to match Seeeduino:nrf52 core
# - Remove test_over_serial code that causes linker errors
#
# Usage:
#   bash tools/setup_tflite_lib.sh
#   bash tools/setup_tflite_lib.sh --ci   # non-interactive (auto-reinstall, quiet wget)
#   make setup-tflite

set -euo pipefail

CI_MODE=false
if [[ "${1:-}" == "--ci" ]]; then
    CI_MODE=true
fi

REPO_URL="https://github.com/lakshanthad/tflite-micro-arduino-examples"
# Pinned commit (the fork's main, unchanged since 2022-02-23). Release builds
# must be reproducible (docs/RELEASE_PROCESS.md); override with TFLITE_REF.
TFLITE_REF="${TFLITE_REF:-48a62b6e75862ceca82924e48618f197750b1299}"
LIB_NAME="tflite-micro-arduino-examples"
TMP_DIR=$(mktemp -d)

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

# arduino-cli's user directory. On Windows (Git Bash) it comes back with
# backslashes, which break the embedded Python below; cygpath -m gives C:/...
arduino_user_dir() {
    local d
    d=$(arduino-cli config get directories.user 2>/dev/null | tr -d '') || true
    [ -n "$d" ] || d="$HOME/Arduino"
    if command -v cygpath &>/dev/null; then d=$(cygpath -m "$d"); fi
    echo "$d"
}

# Check arduino-cli
if ! command -v arduino-cli &>/dev/null; then
    echo "ERROR: arduino-cli not found. Install it first:"
    echo "  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh"
    exit 1
fi

# Check if already installed
if arduino-cli lib list 2>/dev/null | grep -qi "tflite\|tensorflowlite\|tensorflow"; then
    echo "TFLite library already installed:"
    arduino-cli lib list 2>/dev/null | grep -i "tflite\|tensorflowlite\|tensorflow"
    echo ""
    if $CI_MODE; then
        echo "CI mode: auto-reinstalling..."
    else
        read -rp "Reinstall? [y/N] " reply
        [[ "$reply" =~ ^[Yy]$ ]] || { echo "Skipped."; exit 0; }
    fi
    # Remove old installation
    LIB_DIR=$(arduino_user_dir)/libraries
    rm -rf "$LIB_DIR/tflite-micro-arduino-examples-main" "$LIB_DIR/Arduino_TensorFlowLite" 2>/dev/null || true
fi

# Enable zip installs if not already enabled
arduino-cli config set library.enable_unsafe_install true 2>/dev/null || true

echo "Downloading TFLite Micro Arduino library..."
# curl first (Git Bash on Windows has no wget); wget otherwise.
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$TMP_DIR/$LIB_NAME.zip" "$REPO_URL/archive/$TFLITE_REF.zip"
elif $CI_MODE; then
    wget -q -O "$TMP_DIR/$LIB_NAME.zip" \
        "$REPO_URL/archive/$TFLITE_REF.zip"
else
    wget -q --show-progress -O "$TMP_DIR/$LIB_NAME.zip" \
        "$REPO_URL/archive/$TFLITE_REF.zip"
fi

echo "Installing via arduino-cli..."
arduino-cli lib install --zip-path "$TMP_DIR/$LIB_NAME.zip"

# Find installed library path
LIB_DIR=$(arduino_user_dir)/libraries
TFLITE_DIR=$(find "$LIB_DIR" -maxdepth 1 -name "*tflite*" -o -name "*TensorFlowLite*" | head -1)

if [ -z "$TFLITE_DIR" ]; then
    echo "WARNING: Could not find installed library path for patching."
    echo "Library may have installed but patches were not applied."
    exit 1
fi

echo ""
echo "Applying patches..."

# Patch 1: Fix board define case mismatch
SETUP_CPP="$TFLITE_DIR/src/tensorflow/lite/micro/system_setup.cpp"
if [ -f "$SETUP_CPP" ]; then
    sed -i 's/ARDUINO_SEEED_XIAO_NRF52840_SENSE/ARDUINO_Seeed_XIAO_nRF52840_Sense/g' "$SETUP_CPP"
    echo "  Fixed board define case mismatch in system_setup.cpp"

    # Patch 2: Remove test_over_serial code from system_setup.cpp
    "${PYTHON:-$(command -v python3 || command -v python)}" -c "
import re
with open('$SETUP_CPP') as f:
    content = f.read()
content = re.sub(r'namespace test_over_serial \{.*?\}  // namespace test_over_serial',
                 '', content, flags=re.DOTALL)
with open('$SETUP_CPP', 'w') as f:
    f.write(content)
print('  Removed test_over_serial code from system_setup.cpp')
"
fi

# Patch 3: Remove test_over_serial directory (causes linker errors)
TEST_DIR="$TFLITE_DIR/src/test_over_serial"
if [ -d "$TEST_DIR" ]; then
    rm -rf "$TEST_DIR"
    echo "  Removed test_over_serial directory"
fi

echo ""
echo "Verifying installation..."
if arduino-cli lib list 2>/dev/null | grep -qi "tflite\|tensorflowlite"; then
    echo "SUCCESS: TFLite Micro library installed and patched."
    arduino-cli lib list 2>/dev/null | grep -i "tflite\|tensorflowlite"
else
    echo "WARNING: Library may have installed under a different name."
    echo "Check: arduino-cli lib list"
fi

echo ""
echo "TFLite library installed. You can now compile:"
echo "  make compile"
echo "  make uf2-upload UPLOAD_PORT=/dev/ttyACM0"
