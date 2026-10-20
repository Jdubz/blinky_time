#!/usr/bin/env bash
# Apply blinky_time's patches to the pinned Seeeduino nRF52 core in the
# isolated toolchain (<repo>/.arduino, see scripts/arduino_env.sh).
#
#   bash scripts/apply_core_patches.sh            # apply where needed
#   bash scripts/apply_core_patches.sh --check    # report only; exit 1 if unpatched
#
# Patches (each applied only if its guard is missing, so this is idempotent):
#   tinyusb-cdc-no-overwritable-fifo.patch  TX FIFO never overwritable: stock
#       TinyUSB silently drops all serial output once DTR drops
#   tinyusb-cdc-patched-marker.patch        defines BLINKY_TINYUSB_CDC_PATCHED
#       in cdc_device.h; the firmware #errors without it, so a build that
#       links any other (unpatched) TinyUSB fails at compile time (#252)
#
# The core ships CRLF sources. Git Bash's patch strips CRs on read, but GNU
# patch on Linux doesn't, and --ignore-whitespace only covers spaces/tabs, so
# targets are normalized to LF first (<file>.orig keeps the original).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
source scripts/arduino_env.sh

CHECK_ONLY=false
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=true

if [[ ! -d "$BLINKY_CORE_DIR" ]]; then
    echo "ERROR: pinned core not installed at $BLINKY_CORE_DIR (run scripts/setup_toolchain.sh)" >&2
    exit 1
fi

TUSB="libraries/Adafruit_TinyUSB_Arduino/src/class/cdc"
# patch file | target (relative to the core) | guard (ERE that must match once patched)
PATCHES=(
    "patches/tinyusb-cdc-no-overwritable-fifo.patch|$TUSB/cdc_device.c|tu_fifo_set_overwritable\(&p_cdc->tx_ff,[[:space:]]*false\)"
    "patches/tinyusb-cdc-patched-marker.patch|$TUSB/cdc_device.h|^#define BLINKY_TINYUSB_CDC_PATCHED 1"
)

UNPATCHED=0
for entry in "${PATCHES[@]}"; do
    IFS='|' read -r patch rel guard <<< "$entry"
    f="$BLINKY_CORE_DIR/$rel"
    name="$(basename "$patch")"
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $BLINKY_CORE_VERSION: $rel not found" >&2
        exit 1
    fi
    if grep -Eq "$guard" "$f"; then
        echo "  $name: applied"
        continue
    fi
    if $CHECK_ONLY; then
        echo "  $name: NOT applied" >&2
        UNPATCHED=1
        continue
    fi
    [[ -f "$f.orig" ]] || cp "$f" "$f.orig"
    tr -d '\r' < "$f" > "$f.lf" && mv "$f.lf" "$f"
    if ! ( cd "$BLINKY_CORE_DIR" && patch -p1 --ignore-whitespace --forward --no-backup-if-mismatch --quiet < "$BLINKY_ROOT/$patch" ); then
        echo "ERROR: $name did not apply to $rel (original kept at $f.orig)." >&2
        [[ -f "$f.rej" ]] && { echo "--- reject:" >&2; cat "$f.rej" >&2; }
        exit 1
    fi
    grep -Eq "$guard" "$f" || { echo "ERROR: $name applied, but its guard still fails" >&2; exit 1; }
    echo "  $name: applied now"
done

if [[ $UNPATCHED -ne 0 ]]; then
    echo "ERROR: core patches missing (run scripts/setup_toolchain.sh)" >&2
    exit 1
fi
exit 0
