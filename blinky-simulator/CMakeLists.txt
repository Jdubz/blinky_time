cmake_minimum_required(VERSION 3.16)
project(blinky-simulator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific settings
if(WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../blinky-things
)

# Blinky-things source files (only what we need for rendering)
set(BLINKY_SOURCES
    ../blinky-things/types/PixelMatrix.cpp
    ../blinky-things/generators/Fire.cpp
    ../blinky-things/generators/Water.cpp
    ../blinky-things/generators/Lightning.cpp
    ../blinky-things/effects/HueRotationEffect.cpp
    ../blinky-things/render/EffectRenderer.cpp
    ../blinky-things/render/RenderPipeline.cpp
)

# Simulator sources
set(SIMULATOR_SOURCES
    src/main.cpp
)

# Create executable
add_executable(blinky-simulator
    ${SIMULATOR_SOURCES}
    ${BLINKY_SOURCES}
)

# Compiler warnings
if(MSVC)
    target_compile_options(blinky-simulator PRIVATE /W3)
else()
    target_compile_options(blinky-simulator PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Installation
install(TARGETS blinky-simulator DESTINATION bin)

# Print configuration
message(STATUS "blinky-simulator configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
