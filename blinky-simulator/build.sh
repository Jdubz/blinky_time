#!/bin/bash
# Build script for blinky-simulator on Unix/macOS

set -e

echo "=== blinky-simulator build script ==="

# Check for C++ compiler
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "ERROR: No C++ compiler found"
    echo ""
    echo "Please install one of the following:"
    echo "  - GCC: sudo apt install g++ (Ubuntu/Debian)"
    echo "  - Clang: sudo apt install clang (Ubuntu/Debian)"
    echo "  - Xcode Command Line Tools: xcode-select --install (macOS)"
    exit 1
fi

# Choose compiler
if command -v g++ &> /dev/null; then
    CXX=g++
else
    CXX=clang++
fi

echo "Using compiler: $CXX"

# Create build directory
mkdir -p build

# Compile
$CXX -std=c++17 -O2 -Wall -Wextra \
    -I include \
    -I ../blinky-things \
    -o build/blinky-simulator \
    src/main.cpp \
    ../blinky-things/types/PixelMatrix.cpp \
    ../blinky-things/generators/Fire.cpp \
    ../blinky-things/generators/Water.cpp \
    ../blinky-things/generators/Lightning.cpp \
    ../blinky-things/effects/HueRotationEffect.cpp \
    ../blinky-things/render/EffectRenderer.cpp \
    ../blinky-things/render/RenderPipeline.cpp

echo ""
echo "Build successful: build/blinky-simulator"
echo ""
echo "Run with: ./build/blinky-simulator --help"
