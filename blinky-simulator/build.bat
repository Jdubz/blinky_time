@echo off
REM Build script for blinky-simulator on Windows
REM Requires Visual Studio or MinGW-w64 with g++

setlocal enabledelayedexpansion

echo === blinky-simulator build script ===

REM Try to find a C++ compiler
where cl >nul 2>&1 && goto :msvc
where g++ >nul 2>&1 && goto :mingw

echo ERROR: No C++ compiler found in PATH
echo.
echo Please install one of the following:
echo   - Visual Studio with C++ workload (cl.exe)
echo   - MinGW-w64 (g++.exe)
echo.
echo Or add the compiler to your PATH environment variable.
exit /b 1

:msvc
echo Using MSVC (cl.exe)
if not exist build mkdir build

cl /EHsc /std:c++17 /O2 /Fe:build\blinky-simulator.exe ^
    /I include ^
    /I ..\blinky-things ^
    src\main.cpp ^
    ..\blinky-things\types\PixelMatrix.cpp ^
    ..\blinky-things\math\SimplexNoise.cpp ^
    ..\blinky-things\generators\Fire.cpp ^
    ..\blinky-things\generators\Water.cpp ^
    ..\blinky-things\generators\Lightning.cpp ^
    ..\blinky-things\effects\HueRotationEffect.cpp ^
    ..\blinky-things\render\EffectRenderer.cpp ^
    ..\blinky-things\render\RenderPipeline.cpp ^
    /link /OUT:build\blinky-simulator.exe

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful: build\blinky-simulator.exe
goto :end

:mingw
echo Using MinGW (g++)
if not exist build mkdir build

g++ -std=c++17 -O2 -Wall -Wextra ^
    -I include ^
    -I ../blinky-things ^
    -o build/blinky-simulator.exe ^
    src/main.cpp ^
    ../blinky-things/types/PixelMatrix.cpp ^
    ../blinky-things/math/SimplexNoise.cpp ^
    ../blinky-things/generators/Fire.cpp ^
    ../blinky-things/generators/Water.cpp ^
    ../blinky-things/generators/Lightning.cpp ^
    ../blinky-things/effects/HueRotationEffect.cpp ^
    ../blinky-things/render/EffectRenderer.cpp ^
    ../blinky-things/render/RenderPipeline.cpp

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo.
echo Build successful: build\blinky-simulator.exe

:end
echo.
echo Run with: build\blinky-simulator.exe --help
