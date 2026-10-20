# Source this file (don't run it): isolated arduino-cli environment (#252).
#
#   source scripts/arduino_env.sh
#
# Every firmware build uses ONLY the pinned toolchain installed under
# <repo>/.arduino by scripts/setup_toolchain.sh: the Seeeduino core (with its
# bundled, patched TinyUSB) and exactly the libraries in
# tools/firmware_toolchain.json. Nothing in a machine's own Arduino folders
# (e.g. Documents/Arduino/libraries) can shadow a library. That shadowing
# put an unpatched user-installed TinyUSB 3.4.5 into b209-b213.
#
# arduino-cli reads its directories from these environment variables, which
# override any user config. On Windows (Git Bash) arduino-cli needs native
# paths, so they are converted with cygpath -m.

BLINKY_ROOT="$(git rev-parse --show-toplevel)"
BLINKY_ARDUINO_DIR="$BLINKY_ROOT/.arduino"
BLINKY_ARDUINO_PACKAGES="$BLINKY_ARDUINO_DIR/data/packages"
BLINKY_ARDUINO_LIBRARIES="$BLINKY_ARDUINO_DIR/user/libraries"
BLINKY_TOOLCHAIN_JSON="$BLINKY_ROOT/tools/firmware_toolchain.json"

_blinky_native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi
}

BLINKY_PYTHON="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)}"
_blinky_tc() {
    "$BLINKY_PYTHON" -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" \
        "$(_blinky_native "$BLINKY_TOOLCHAIN_JSON")" "$1"
}

export ARDUINO_CONFIG_FILE="$(_blinky_native "$BLINKY_ARDUINO_DIR/arduino-cli.yaml")"
export ARDUINO_DIRECTORIES_DATA="$(_blinky_native "$BLINKY_ARDUINO_DIR/data")"
export ARDUINO_DIRECTORIES_USER="$(_blinky_native "$BLINKY_ARDUINO_DIR/user")"
export ARDUINO_DIRECTORIES_DOWNLOADS="$(_blinky_native "$BLINKY_ARDUINO_DIR/staging")"
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS="$(_blinky_tc board_index_url)"
export ARDUINO_LIBRARY_ENABLE_UNSAFE_INSTALL=true
export ARDUINO_UPDATER_ENABLE_NOTIFICATION=false

BLINKY_FQBN="$(_blinky_tc fqbn)"
BLINKY_CORE="$(_blinky_tc core)"            # e.g. Seeeduino:nrf52@1.1.10
BLINKY_CORE_VERSION="${BLINKY_CORE##*@}"
BLINKY_CORE_DIR="$BLINKY_ARDUINO_PACKAGES/Seeeduino/hardware/nrf52/$BLINKY_CORE_VERSION"
BLINKY_ARDUINO_CLI_VERSION="$(_blinky_tc arduino_cli)"

# The arduino-cli version is part of the toolchain too (CI's
# setup-arduino-cli used to install the newest release, even a release
# candidate). Validate the executable that will actually compile:
#   blinky_require_cli_version [cli]   (default: $ARDUINO_CLI or arduino-cli)
blinky_require_cli_version() {
    local cli="${1:-${ARDUINO_CLI:-arduino-cli}}" have
    have="$("$cli" version 2>/dev/null | sed -n 's/.*Version: *\([^ ]*\).*/\1/p')"
    if [[ "$have" != "$BLINKY_ARDUINO_CLI_VERSION" ]]; then
        echo "ERROR: $cli is arduino-cli '$have', pinned $BLINKY_ARDUINO_CLI_VERSION (tools/firmware_toolchain.json)." >&2
        echo "       Install arduino-cli $BLINKY_ARDUINO_CLI_VERSION: https://arduino.github.io/arduino-cli/" >&2
        return 1
    fi
}

# Fail loudly if the isolated toolchain isn't installed (don't fall back to
# whatever the machine has).
blinky_require_toolchain() {
    blinky_require_cli_version "${1:-}" || return 1
    if [[ ! -d "$BLINKY_CORE_DIR" || ! -f "$BLINKY_ARDUINO_DIR/arduino-cli.yaml" ]]; then
        echo "ERROR: pinned toolchain not installed in $BLINKY_ARDUINO_DIR." >&2
        echo "       Run: bash scripts/setup_toolchain.sh   (see docs/RELEASE_PROCESS.md)" >&2
        return 1
    fi
}
