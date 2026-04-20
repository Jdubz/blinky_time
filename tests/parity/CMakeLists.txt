cmake_minimum_required(VERSION 3.16)
project(parity_harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# BLINKY_PARITY_HARNESS swaps out the FFT include in SharedSpectralAnalysis.cpp
# so we don't need arm_math.h or arduinoFFT.h on the host. The harness injects
# magnitudes directly and never calls computeFFT(); if it ever does, the
# guarded no-op path will hit and the test should fail with a clear error.
add_definitions(-DBLINKY_PARITY_HARNESS)
# Platform detection expects one of the known Arduino macros. We claim
# nRF52840 — the SharedSpectralAnalysis code paths gated by NRF52840 are
# all either (a) inside the BLINKY_PARITY_HARNESS guard above, or (b) pure
# math that's identical across platforms. Same pattern the simulator uses.
add_definitions(-DARDUINO_ARCH_NRF52 -DBLINKY_PLATFORM_NRF52840)

# The simulator's ArduinoCompat.h stubs F(), __FlashStringHelper, Serial, etc.
# BlinkyAssert transitively includes Arduino.h, so we reuse the simulator's
# drop-in replacement rather than maintain a second compatibility layer.
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../blinky-simulator/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../blinky-things
)

# Firmware sources needed to satisfy the linker. Keep this set minimal —
# each addition couples the parity test to more production code.
set(BLINKY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../blinky-things/audio/SharedSpectralAnalysis.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../blinky-things/types/BlinkyAssert.cpp
)

add_executable(parity_harness
    parity_harness.cpp
    ${BLINKY_SOURCES}
)

if(MSVC)
    target_compile_options(parity_harness PRIVATE /W3)
else()
    target_compile_options(parity_harness PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

message(STATUS "parity_harness: BLINKY_PARITY_HARNESS defined; FFT path stubbed")
