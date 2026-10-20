#!/usr/bin/env bash
# Install the pinned firmware toolchain into <repo>/.arduino (#252).
#
#   bash scripts/setup_toolchain.sh
#
# Every machine that builds firmware (workstations, blinkyhost, CI, the
# release workflow) runs this, so every build resolves exactly the same core
# and libraries: tools/firmware_toolchain.json, TFLite pinned in
# tools/setup_tflite_lib.sh, the core patches in patches/. scripts/build.sh
# and the Makefile build only inside this environment.
#
# Idempotent. Fails loudly if anything is missing, extra, or unpatched.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
source scripts/arduino_env.sh

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "ERROR: arduino-cli not found on PATH" >&2
    exit 1
fi

mkdir -p "$BLINKY_ARDUINO_DIR/data" "$BLINKY_ARDUINO_DIR/user/libraries" "$BLINKY_ARDUINO_DIR/staging"
# Directories and URLs come from the environment (arduino_env.sh); the config
# file only exists so no user-level config file is read instead.
printf 'directories: {}\n' > "$BLINKY_ARDUINO_DIR/arduino-cli.yaml"

echo "=== Pinned toolchain -> $BLINKY_ARDUINO_DIR ==="
arduino-cli core update-index
arduino-cli core install "$BLINKY_CORE"

mapfile -t LIBS < <("$BLINKY_PYTHON" -c "import json,sys; print('\n'.join(json.load(open(sys.argv[1]))['libraries']))" \
                    "$(_blinky_native "$BLINKY_TOOLCHAIN_JSON")" | tr -d '\r')
for lib in "${LIBS[@]}"; do
    arduino-cli lib install "$lib"
done

bash tools/setup_tflite_lib.sh --ci
bash scripts/apply_core_patches.sh

# Verify: exactly the pinned libraries (plus TFLite), nothing else.
"$BLINKY_PYTHON" - "$(_blinky_native "$BLINKY_TOOLCHAIN_JSON")" <<'EOF'
import json, subprocess, sys
want = {}
for spec in json.load(open(sys.argv[1]))["libraries"]:
    name, ver = spec.rsplit("@", 1)
    want[name] = ver
out = subprocess.run(["arduino-cli", "lib", "list", "--format", "json"],
                     capture_output=True, text=True, encoding="utf-8", errors="replace",
                     check=True).stdout
data = json.loads(out or "{}")
rows = data.get("installed_libraries", data) if isinstance(data, dict) else data
have = {}
for r in rows or []:
    lib = r.get("library", r)
    have[lib["name"]] = lib.get("version", "")
bad = []
for name, ver in want.items():
    if have.get(name) != ver:
        bad.append(f"{name}: want {ver}, have {have.get(name)}")
extra = sorted(set(have) - set(want) - {"Arduino_TensorFlowLite"})
if extra:
    bad.append(f"unexpected libraries: {extra}")
if "Arduino_TensorFlowLite" not in have:
    bad.append("Arduino_TensorFlowLite missing")
if bad:
    sys.exit("ERROR: toolchain library set wrong:\n  " + "\n  ".join(bad))
print("  libraries:", ", ".join(f"{k}@{v}" for k, v in sorted(have.items())))
EOF
bash scripts/apply_core_patches.sh --check
echo "=== Toolchain ready ($BLINKY_CORE) ==="
