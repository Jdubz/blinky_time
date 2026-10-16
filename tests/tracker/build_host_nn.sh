#!/bin/bash
# Build the tracker harness with the REAL onset NN (#203).
#
# Compiles the Arduino TFLite Micro library for the host (reference kernels +
# CMSIS-NN's portable C fallbacks), then links tests/tracker/tracker_harness.cpp
# against it with BLINKY_HOST_NN, so FrameOnsetNN runs the embedded model
# (blinky-things/audio/frame_onset_model_data.h) exactly as the device does.
#
# Usage: tests/tracker/build_host_nn.sh [--lib <library src dir>] [--out <dir>]
#   --lib  defaults to the TFLite library installed for arduino-cli
#          (tools/setup_tflite_lib.sh), searched under its user libraries dir.
#   --out  defaults to build/host_nn
#   CXX / AR override the compiler / archiver (e.g. CXX="python -m ziglang c++",
#   AR="python -m ziglang ar"); default c++ / ar. Output: <out>/tracker_harness_nn
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

LIB=""; OUT="build/host_nn"
while [ $# -gt 0 ]; do
    case "$1" in
        --lib) LIB="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
CXX="${CXX:-c++}"; AR="${AR:-ar}"

if [ -z "$LIB" ]; then
    LIBROOT=$(arduino-cli config get directories.user 2>/dev/null || echo "$HOME/Arduino")/libraries
    for cand in "$LIBROOT"/Arduino_TensorFlowLite "$LIBROOT"/tflite-micro-arduino-examples*; do
        if [ -f "$cand/src/TensorFlowLite.h" ]; then LIB="$cand/src"; break; fi
    done
fi
if [ -z "$LIB" ] || [ ! -f "$LIB/TensorFlowLite.h" ]; then
    echo "ERROR: TFLite Micro library not found (run tools/setup_tflite_lib.sh, or pass --lib <dir>/src)" >&2
    exit 1
fi
echo "TFLite Micro: $LIB"

# Fresh object dir every run: objects from sources that were removed or
# renamed must not end up in the archive.
rm -rf "$OUT/obj"
mkdir -p "$OUT/obj"
INC=(-I"$LIB" -I"$LIB/third_party/flatbuffers/include" -I"$LIB/third_party/gemmlowp"
     -I"$LIB/third_party/ruy" -I"$LIB/third_party/cmsis" -I"$LIB/third_party/cmsis/CMSIS/NN/Include"
     -I"$LIB/third_party/cmsis/CMSIS/DSP/Include" -I"$LIB/third_party/cmsis/CMSIS/Core/Include")
# ARDUINO matches what the device build sees (it selects the Arduino kernel
# variants, e.g. FULLY_CONNECTED_INT8). No ARM intrinsics are defined, so
# CMSIS-NN compiles its portable C paths.
DEFS=(-DARDUINO=10819 -DTF_LITE_STATIC_MEMORY -DCMSIS_NN -DNDEBUG)
# Same as the device build: no exceptions / RTTI. With exceptions enabled GCC
# rejects TFLite's placement-new of classes with a private operator delete
# (TF_LITE_REMOVE_VIRTUAL_DELETE, active under ARDUINO).
CXXFLAGS_DEV=(-fno-exceptions -fno-rtti)

# Library sources, minus tests/examples, NPU glue and the Arduino-Serial
# system_setup.cpp (replaced by a host stub below).
# Filter on paths RELATIVE to the library, so a parent directory named
# arduino/ or examples/ can't drop every source.
( cd "$LIB" && find tensorflow third_party/cmsis/CMSIS/NN/Source \( -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
    | grep -vE "_test\.|(^|/)examples/|ethosu|(^|/)arduino/|^tensorflow/lite/micro/system_setup\.cpp$" \
    | sort | sed "s|^|$LIB/|" ) > "$OUT/srcs.txt"
[ -s "$OUT/srcs.txt" ] || { echo "ERROR: no TFLite sources found under $LIB" >&2; exit 1; }

cat > "$OUT/host_system_setup.cpp" <<'EOF'
// Host stand-in for TFLite Micro's micro/system_setup.cpp (Arduino Serial).
#include <cstdio>
#include "tensorflow/lite/micro/system_setup.h"
extern "C" void DebugLog(const char* s) { std::fputs(s, stderr); }
namespace tflite { void InitializeTarget() {} }
EOF

compile_one() {
    local src="$1" obj lang
    obj="$OUT/obj/$(echo "$src" | sed 's|.*/src/||; s|[/.]|_|g').o"
    case "$src" in *.c) lang=(-x c -std=c11) ;; *) lang=(-std=c++17 "${CXXFLAGS_DEV[@]}") ;; esac
    # Always rebuilt: a timestamp cache can't see header, --lib or CXX changes.
    $CXX "${lang[@]}" -O2 -c "${DEFS[@]}" "${INC[@]}" "$src" -o "$obj" || { echo "FAILED: $src" >&2; return 1; }
}
FAIL=0
while read -r src; do compile_one "$src" || FAIL=1; done < "$OUT/srcs.txt"
[ $FAIL -eq 0 ] || { echo "ERROR: TFLite Micro host build failed" >&2; exit 1; }

( cd "$OUT/obj" && ls ./*.o > ../objs.rsp && rm -f ../libtflm_host.a && $AR rcs ../libtflm_host.a @../objs.rsp )

$CXX -std=c++17 "${CXXFLAGS_DEV[@]}" -O2 -DBLINKY_HOST_HARNESS -DBLINKY_HOST_NN -DARDUINO_ARCH_NRF52 \
    -DBLINKY_PLATFORM_NRF52840 "${DEFS[@]}" -Iblinky-simulator/include -Iblinky-things "${INC[@]}" \
    tests/tracker/tracker_harness.cpp blinky-things/audio/AudioTracker.cpp \
    blinky-things/audio/SharedSpectralAnalysis.cpp blinky-things/inputs/AdaptiveMic.cpp \
    blinky-things/types/BlinkyAssert.cpp "$OUT/host_system_setup.cpp" "$OUT/libtflm_host.a" \
    -o "$OUT/tracker_harness_nn"
echo "Built $OUT/tracker_harness_nn"
