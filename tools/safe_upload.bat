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

set ARDUINO_CLI=D:\Development\Arduino\arduino-cli.exe
set FQBN=Seeeduino:mbed:xiaonRF52840Sense
set SKETCH_DIR=D:\Development\Arduino\blinky_time\blinky-things
set HEX_FILE=%SKETCH_DIR%\build\Seeeduino.mbed.xiaonRF52840Sense\blinky-things.ino.hex
set TOOLS_DIR=D:\Development\Arduino\blinky_time\tools

echo.
echo ============================================================
echo   SAFE BUILD - Blinky Things
echo ============================================================
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
