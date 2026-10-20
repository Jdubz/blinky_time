#!/bin/bash
# Build firmware with auto-incrementing build number.
#
# Usage:
#   ./scripts/build.sh                    # compile only (nRF52840)
#   ./scripts/build.sh --upload PORT      # compile + upload via UF2
#   ./scripts/build.sh --esp32 [--upload PORT]  # ESP32-S3 target
#   ./scripts/build.sh --no-bump          # compile without incrementing
#   ./scripts/build.sh --dev              # local dev build: no bump, version b<N>-dev-<sha>
#   ./scripts/build.sh --build N          # exact build number, no bump (release CI)
#
# Releases are built by .github/workflows/release.yml, not locally; see
# docs/RELEASE_PROCESS.md. Local builds are for one test device only.
#
# The build number in blinky-things/BUILD_NUMBER is incremented on each
# compile and baked into the firmware via types/Version.h. This is the
# sole firmware version identifier — not tied to git tags or branches.
#
# Note: not safe for concurrent builds (two simultaneous runs could
# produce the same build number). Fine for single-developer use.

set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

BUMP=true
DEV=false
FORCE_BUILD=""
ESP32=false
UPLOAD=false
UPLOAD_PORT=""
OUTPUT_DIR="/tmp/blinky-build"
ARDUINO_CLI="${ARDUINO_CLI:-$(command -v arduino-cli 2>/dev/null || echo "$HOME/bin/arduino-cli")}"

while [[ $# -gt 0 ]]; do
    case $1 in
        --no-bump)   BUMP=false; shift ;;
        --dev)       DEV=true; BUMP=false; shift ;;
        --build)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]+$ ]]; then
                echo "Error: --build requires a build number" >&2; exit 1
            fi
            FORCE_BUILD="$2"; BUMP=false; shift 2 ;;
        --esp32)     ESP32=true; shift ;;
        --upload)
            UPLOAD=true
            if [[ $# -lt 2 || "$2" == --* ]]; then
                echo "Error: --upload requires a port argument (e.g., --upload /dev/ttyACM0)" >&2
                exit 1
            fi
            UPLOAD_PORT="$2"; shift 2 ;;
        *)           echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if ! command -v "$ARDUINO_CLI" >/dev/null 2>&1; then
    echo "Error: arduino-cli not found. Install it or set ARDUINO_CLI env var." >&2
    exit 1
fi

# --- Locate python (python3 on Linux/macOS, python on Windows) ---
PYTHON="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo "python3")}"

# --- Pinned, isolated toolchain (#252) ---
# nRF52840 builds use ONLY <repo>/.arduino (scripts/setup_toolchain.sh): the
# pinned core with its patched TinyUSB and exactly the pinned libraries.
# Nothing in this machine's own Arduino folders can shadow a library (a
# user-installed, unpatched TinyUSB 3.4.5 got into b209-b213 that way).
# ESP32-S3 (next gen) is not under the pinned toolchain yet and builds with
# the machine's own arduino-cli setup.
if ! $ESP32; then
    source scripts/arduino_env.sh
    blinky_require_toolchain || exit 1
    # Both core patches present (the firmware also #errors without the
    # marker, so this is a fast, explicit first check).
    bash scripts/apply_core_patches.sh --check || exit 1
fi

# --- Read build number ---
BUILD_FILE="blinky-things/BUILD_NUMBER"
BUILD=$(cat "$BUILD_FILE" | tr -d '[:space:]')
if ! [[ "$BUILD" =~ ^[0-9]+$ ]]; then
    echo "Error: BUILD_NUMBER is not a valid integer: '$BUILD'" >&2
    exit 1
fi

if $BUMP; then
    BUILD=$((BUILD + 1))
fi
if [[ -n "$FORCE_BUILD" ]]; then
    BUILD="$FORCE_BUILD"
fi

# --- Generate Version.h (single generator shared with the Makefile, #213) ---
if $DEV; then
    bash scripts/gen_version.sh "$BUILD" --dev
else
    bash scripts/gen_version.sh "$BUILD"
fi

# --- Select FQBN ---
if $ESP32; then
    FQBN='esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,PSRAM=opi'
else
    FQBN='Seeeduino:nrf52:xiaonRF52840Sense'
fi

# --- Compile ---
COMPILE_ARGS=(
    compile
    --fqbn "$FQBN"
    --output-dir "$OUTPUT_DIR"
)

if $UPLOAD && $ESP32; then
    COMPILE_ARGS=(compile --upload --fqbn "$FQBN" -p "$UPLOAD_PORT")
fi

if $ESP32; then
    "$ARDUINO_CLI" "${COMPILE_ARGS[@]}" blinky-things
else
    # --verbose so the library resolution can be checked; the full log is
    # kept, only the summary is shown.
    mkdir -p "$OUTPUT_DIR"
    COMPILE_LOG="$OUTPUT_DIR/compile.log"
    if ! "$ARDUINO_CLI" "${COMPILE_ARGS[@]}" --verbose blinky-things > "$COMPILE_LOG" 2>&1; then
        grep -E "error|Error" "$COMPILE_LOG" | head -40 >&2
        echo "Compilation failed (full log: $COMPILE_LOG)" >&2
        exit 1
    fi
    grep -E "^Sketch uses|^Global variables" "$COMPILE_LOG" || true
    "$PYTHON" tools/check_build_libs.py "$COMPILE_LOG" --out "$OUTPUT_DIR/libraries.json" || exit 1
fi

# Save build number only AFTER successful compile (no gaps on failure)
if $BUMP; then
    echo "$BUILD" > "$BUILD_FILE"
fi

echo "=== Build $BUILD complete ==="

# --- Upload via UF2 (nRF52840 only) ---
if $UPLOAD && ! $ESP32; then
    echo "=== Uploading to $UPLOAD_PORT via UF2 ==="
    # Windows: uf2_upload.py uses Linux-specific drive detection (lsblk, udisksctl).
    # Use uf2_upload_win.py instead — it detects the XIAO-SENSE drive via Win32_Volume.
    # NOTE: On Windows the software bootloader trigger (serial command / 1200-baud touch)
    # is unreliable because the Windows USB host power-cycles the port after
    # NVIC_SystemReset(), clearing the RAM magic before the bootloader can read it.
    # Double-tap the reset button on the device before running with --upload, then
    # re-run with --upload to copy the UF2 to the already-mounted XIAO-SENSE drive.
    if [[ -n "${LOCALAPPDATA:-}" ]]; then
        "$PYTHON" tools/uf2_upload_win.py "$UPLOAD_PORT" --build-dir "$OUTPUT_DIR"
    else
        "$PYTHON" tools/uf2_upload.py "$UPLOAD_PORT" --build-dir "$OUTPUT_DIR"
    fi
fi
