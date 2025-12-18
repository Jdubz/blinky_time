@echo off
REM Safe Build Script for Blinky Things
REM Compiles and validates firmware safety - USE ARDUINO IDE FOR UPLOAD
REM
REM WARNING: arduino-cli upload has been found to corrupt the bootloader
REM on Seeeduino XIAO nRF52840 boards. Always use Arduino IDE for upload.
REM
REM Usage: safe_upload.bat
REM
REM This script:
REM 1. Compiles the firmware
REM 2. Runs pre-upload safety validation
REM 3. Instructs user to upload via Arduino IDE

setlocal enabledelayedexpansion

REM --- CONFIGURATION ---
set "FQBN=Seeeduino:mbed:xiaonRF52840Sense"

REM --- SCRIPT LOGIC (should not need changes) ---
REM Get the directory of this script
set "SCRIPT_DIR=%~dp0"
REM Go up one level to the repo root
for %%i in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fi"

set "SKETCH_DIR=%REPO_ROOT%\blinky-things"
set "TOOLS_DIR=%REPO_ROOT%\tools"

REM Convert FQBN to path format for build directory (replace : with .)
set "FQBN_PATH=%FQBN::=.%"
set "HEX_FILE=%SKETCH_DIR%\build\%FQBN_PATH%\blinky-things.ino.hex"

REM Find arduino-cli in PATH
set "ARDUINO_CLI="
for %%i in (arduino-cli.exe) do set "ARDUINO_CLI=%%~$PATH:i"
if not defined ARDUINO_CLI (
    echo ERROR: arduino-cli.exe not found in your system PATH.
    echo Please install it and add it to your PATH, or set ARDUINO_CLI manually.
    echo.
    echo Installation: https://arduino.github.io/arduino-cli/installation/
    exit /b 1
)

echo.
echo ============================================================
echo   SAFE BUILD - Blinky Things
echo ============================================================
echo.
echo Using arduino-cli: %ARDUINO_CLI%
echo Sketch: %SKETCH_DIR%
echo.

REM Step 1: Compile
echo [1/2] Compiling firmware...
"%ARDUINO_CLI%" compile --fqbn %FQBN% "%SKETCH_DIR%"
if errorlevel 1 (
    echo.
    echo COMPILATION FAILED
    exit /b 1
)

echo.
echo [2/2] Running pre-upload safety validation...
echo.

REM Step 2: Safety validation
py "%TOOLS_DIR%\pre_upload_check.py" "%HEX_FILE%"
if errorlevel 1 (
    echo.
    echo ============================================================
    echo   SAFETY CHECK FAILED - DO NOT UPLOAD
    echo ============================================================
    echo.
    echo The firmware did not pass safety validation.
    echo Review the errors above before proceeding.
    echo.
    exit /b 1
)

echo.
echo ============================================================
echo   BUILD COMPLETE - Ready for upload
echo ============================================================
echo.
echo !!! USE ARDUINO IDE TO UPLOAD !!!
echo.
echo arduino-cli upload has a bug that corrupts the bootloader
echo on Seeeduino XIAO nRF52840 boards. Use Arduino IDE instead:
echo.
echo   1. Open Arduino IDE
echo   2. Open: %SKETCH_DIR%
echo   3. Select board: Seeed XIAO BLE Sense - nRF52840
echo   4. Select port and click Upload
echo.

exit /b 0
