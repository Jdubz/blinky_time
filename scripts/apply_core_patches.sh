#!/usr/bin/env bash
# Apply blinky_time's patches to every installed Seeeduino nRF52 core.
#
# scripts/build.sh refuses to compile until the TinyUSB CDC patch is in
# place (stock TinyUSB silently drops all serial output once DTR drops).
# Run this after installing or upgrading the core:
#
#   bash scripts/apply_core_patches.sh            # apply where needed
#   bash scripts/apply_core_patches.sh --check    # report only; exit 1 if unpatched
#
# Idempotent: an already-patched core is left alone. The first patch keeps
# the original next to the file as <file>.orig. The core ships CRLF sources,
# so the patch is applied with --ignore-whitespace (a CRLF context line
# matches the LF patch); patched lines come out LF, which compilers ignore.
# Fails loudly if the patch doesn't apply or the guard pattern build.sh
# checks for is still missing afterwards.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

CHECK_ONLY=false
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=true

PATCH="$PWD/patches/tinyusb-cdc-no-overwritable-fifo.patch"
REL="libraries/Adafruit_TinyUSB_Arduino/src/class/cdc/cdc_device.c"
# Same assertion as build.sh's compile guard.
GUARD='tu_fifo_set_overwritable\(&p_cdc->tx_ff,[[:space:]]*false\)'

# Arduino15 packages dir, located the same way as build.sh.
if [[ -d "$HOME/.arduino15/packages" ]]; then
    PKGS="$HOME/.arduino15/packages"
elif [[ -n "${LOCALAPPDATA:-}" ]]; then
    if command -v cygpath >/dev/null 2>&1; then
        PKGS="$(cygpath -u "$LOCALAPPDATA")/Arduino15/packages"
    else
        PKGS="${LOCALAPPDATA//\\//}/Arduino15/packages"
    fi
else
    echo "ERROR: no Arduino15 packages directory found" >&2
    exit 1
fi

shopt -s nullglob
CORES=("$PKGS"/Seeeduino/hardware/nrf52/*/)
if [[ ${#CORES[@]} -eq 0 ]]; then
    echo "ERROR: no Seeeduino nRF52 core under $PKGS (arduino-cli core install Seeeduino:nrf52)" >&2
    exit 1
fi

UNPATCHED=0
for core in "${CORES[@]}"; do
    core="${core%/}"
    f="$core/$REL"
    ver="$(basename "$core")"
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $ver: $REL not found" >&2
        exit 1
    fi
    if grep -Eq "$GUARD" "$f"; then
        echo "  $ver: already patched"
        continue
    fi
    if $CHECK_ONLY; then
        echo "  $ver: NOT patched"
        UNPATCHED=1
        continue
    fi
    [[ -f "$f.orig" ]] || cp "$f" "$f.orig"
    if ! ( cd "$core" && patch -p1 --ignore-whitespace --forward --no-backup-if-mismatch --quiet < "$PATCH" ); then
        echo "ERROR: $ver: patch did not apply. The core's cdc_device.c changed; regenerate" >&2
        echo "       $PATCH against it (original kept at $f.orig)." >&2
        exit 1
    fi
    grep -Eq "$GUARD" "$f" || { echo "ERROR: $ver: patched, but the build.sh guard still fails" >&2; exit 1; }
    echo "  $ver: patched"
done

$CHECK_ONLY && [[ $UNPATCHED -ne 0 ]] && exit 1
exit 0
