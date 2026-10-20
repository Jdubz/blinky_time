#!/usr/bin/env bash
# Compile inside the pinned, isolated firmware toolchain (#252), with the
# same checks scripts/build.sh applies.
#
#   bash scripts/in_toolchain.sh <arduino-cli> compile [args...] <sketch>
#
# Used by the Makefile's nRF52840 compile targets so `make compile` /
# `make uf2-upload` can't accept a build that build.sh would reject:
# - the arduino-cli that compiles is the pinned version,
# - the toolchain is installed and both core patches are applied,
# - every library resolves inside .arduino/ at its pinned version
#   (tools/check_build_libs.py on the verbose compile log).
set -euo pipefail
CLI="$1"; shift
[[ "${1:-}" == "compile" ]] || { echo "in_toolchain.sh: only '<cli> compile ...' is supported" >&2; exit 2; }
source "$(git rev-parse --show-toplevel)/scripts/arduino_env.sh"
blinky_require_toolchain "$CLI"
bash "$BLINKY_ROOT/scripts/apply_core_patches.sh" --check >/dev/null

LOG_DIR="$(mktemp -d)"
LOG="$LOG_DIR/compile.log"
if ! "$CLI" "$@" --verbose > "$LOG" 2>&1; then
    grep -E "error|Error" "$LOG" | head -40 >&2
    echo "Compilation failed (full log: $LOG)" >&2
    exit 1
fi
grep -E "^Sketch uses|^Global variables" "$LOG" || true
"$BLINKY_PYTHON" "$BLINKY_ROOT/tools/check_build_libs.py" "$LOG" --out "$LOG_DIR/libraries.json"
rm -rf "$LOG_DIR"
