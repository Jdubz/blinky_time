#!/bin/bash
# Build script for blinky-simulator on Unix/macOS

set -e

echo "=== blinky-simulator build script ==="

# Choose compiler: honor $CXX if set, else g++, else clang++
if [ -n "${CXX:-}" ]; then
    if ! command -v "$CXX" &> /dev/null; then
        echo "ERROR: CXX='$CXX' is not an executable command"
        exit 1
    fi
elif command -v g++ &> /dev/null; then
    CXX=g++
elif command -v clang++ &> /dev/null; then
    CXX=clang++
else
    echo "ERROR: No C++ compiler found"
    echo ""
    echo "Please install one of the following (or set CXX):"
    echo "  - GCC: sudo apt install g++ (Ubuntu/Debian)"
    echo "  - Clang: sudo apt install clang (Ubuntu/Debian)"
    echo "  - Xcode Command Line Tools: xcode-select --install (macOS)"
    exit 1
fi

echo "Using compiler: $CXX"

# Create build directory
mkdir -p build

# Compile
$CXX -std=c++17 -O2 -Wall -Wextra \
    -DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840 \
    -I include \
    -I ../blinky-things \
    -o build/blinky-simulator \
    src/main.cpp \
    ../blinky-things/types/PixelMatrix.cpp \
    ../blinky-things/types/BlinkyAssert.cpp \
    ../blinky-things/physics/PhysicsContext.cpp \
    ../blinky-things/particles/Particle.cpp \
    ../blinky-things/math/SimplexNoise.cpp \
    ../blinky-things/generators/Fire.cpp \
    ../blinky-things/generators/Water.cpp \
    ../blinky-things/generators/PlasmaGlobe.cpp \
    ../blinky-things/generators/Audio.cpp \
    ../blinky-things/effects/HueRotationEffect.cpp \
    ../blinky-things/render/EffectRenderer.cpp \
    ../blinky-things/render/RenderPipeline.cpp

echo ""
echo "Build successful: build/blinky-simulator"
echo ""
echo "Run with: ./build/blinky-simulator --help"
