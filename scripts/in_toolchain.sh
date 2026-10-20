#!/usr/bin/env bash
# Run a command inside the pinned, isolated firmware toolchain (#252).
#
#   bash scripts/in_toolchain.sh arduino-cli compile --fqbn ... blinky-things
#
# Used by the Makefile's nRF52840 compile targets so `make compile` /
# `make uf2-upload` resolve exactly what scripts/build.sh does. Fails if
# scripts/setup_toolchain.sh hasn't been run.
set -euo pipefail
source "$(git rev-parse --show-toplevel)/scripts/arduino_env.sh"
blinky_require_toolchain
bash "$BLINKY_ROOT/scripts/apply_core_patches.sh" --check >/dev/null
exec "$@"
