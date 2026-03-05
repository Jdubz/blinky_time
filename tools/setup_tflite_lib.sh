#!/usr/bin/env bash
# Install TFLite Micro Arduino library for XIAO nRF52840 Sense builds.
#
# Uses the Seeed-maintained fork of tensorflow/tflite-micro-arduino-examples
# which is tested on the XIAO BLE Sense (our exact hardware).
#
# Usage:
#   bash tools/setup_tflite_lib.sh
#   make setup-tflite

set -euo pipefail

REPO_URL="https://github.com/lakshanthad/tflite-micro-arduino-examples"
LIB_NAME="tflite-micro-arduino-examples"
TMP_DIR=$(mktemp -d)

cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

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
    read -rp "Reinstall? [y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "Skipped."; exit 0; }
fi

echo "Downloading TFLite Micro Arduino library..."
wget -q --show-progress -O "$TMP_DIR/$LIB_NAME.zip" \
    "$REPO_URL/archive/refs/heads/main.zip"

echo "Installing via arduino-cli..."
arduino-cli lib install --zip-path "$TMP_DIR/$LIB_NAME.zip"

echo ""
echo "Verifying installation..."
if arduino-cli lib list 2>/dev/null | grep -qi "tflite\|tensorflowlite"; then
    echo "SUCCESS: TFLite Micro library installed."
    arduino-cli lib list 2>/dev/null | grep -i "tflite\|tensorflowlite"
else
    echo "WARNING: Library may have installed under a different name."
    echo "Check: arduino-cli lib list"
fi

echo ""
echo "You can now compile with NN beat activation:"
echo "  make compile NN=1"
echo "  make uf2-upload NN=1 UPLOAD_PORT=/dev/ttyACM0"
