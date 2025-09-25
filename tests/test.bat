@echo off
REM Blinky Time Test Runner - Windows Batch Script
REM 
REM Quick test runner for Windows users without Python setup.
REM Tests compilation for all device types and provides regression protection.
REM
REM Usage:
REM   test.bat [device_type] [com_port]
REM   
REM Examples:
REM   test.bat              - Test all device types (compilation only)
REM   test.bat 2            - Test tube light compilation
REM   test.bat 2 COM4       - Test tube light with hardware on COM4
REM
REM Author: Blinky Time Project Contributors
REM License: Creative Commons Attribution-ShareAlike 4.0 International

setlocal enabledelayedexpansion

echo.
echo ========================================
echo 🔥 Blinky Time Test Runner
echo ========================================
echo.

REM Check if Arduino CLI is available
arduino-cli version >nul 2>&1
if errorlevel 1 (
    echo ❌ Arduino CLI not found!
    echo Please install arduino-cli and add to PATH.
    echo Download: https://arduino.github.io/arduino-cli/
    pause
    exit /b 1
)

REM Set default values
set DEVICE_TYPE=%1
set COM_PORT=%2
set TEST_PASSED=true

if "%DEVICE_TYPE%"=="" set DEVICE_TYPE=all

echo 🔧 Arduino CLI found
echo 📁 Project root: %~dp0..
echo 🎯 Testing device type: %DEVICE_TYPE%
if not "%COM_PORT%"=="" echo 🔌 Hardware port: %COM_PORT%
echo.

REM Change to project directory
cd /d "%~dp0..\blinky-things"

REM Function to test compilation for a device type
if "%DEVICE_TYPE%"=="all" (
    call :test_device_compilation 1 "Hat"
    call :test_device_compilation 2 "Tube Light" 
    call :test_device_compilation 3 "Bucket Totem"
) else (
    if "%DEVICE_TYPE%"=="1" call :test_device_compilation 1 "Hat"
    if "%DEVICE_TYPE%"=="2" call :test_device_compilation 2 "Tube Light"
    if "%DEVICE_TYPE%"=="3" call :test_device_compilation 3 "Bucket Totem"
)

REM Hardware testing if COM port specified
if not "%COM_PORT%"=="" (
    echo.
    echo 🧪 Hardware Testing
    echo ==================
    echo Uploading test runner to %COM_PORT%...
    
    REM Upload test runner
    cd /d "%~dp0"
    arduino-cli upload --fqbn Seeeduino:nrf52:xiaonRF52840Sense --port %COM_PORT% . >nul 2>&1
    
    if errorlevel 1 (
        echo ❌ Upload failed - check connection and port
        set TEST_PASSED=false
    ) else (
        echo ✅ Test runner uploaded successfully
        echo 📊 Open Serial Monitor at 115200 baud to view test results
        echo 🔗 Or use: arduino-cli monitor --port %COM_PORT% --config baudrate=115200
    )
)

REM Final results
echo.
echo ========================================
echo 📊 Test Summary
echo ========================================

if "%TEST_PASSED%"=="true" (
    echo ✅ ALL TESTS PASSED!
    echo 💡 Safe to proceed with refactoring
    echo.
    echo Next steps:
    echo - Run hardware tests if you haven't: test.bat %DEVICE_TYPE% COM4
    echo - Check test coverage with: python tests\run_tests.py --report
) else (
    echo ❌ SOME TESTS FAILED!
    echo 🔍 Fix compilation errors before refactoring
    echo 📖 Check error messages above for details
)

echo.
pause
exit /b 0

REM Function to test compilation for specific device type
:test_device_compilation
set dt=%1
set name=%2

echo 🔨 Testing %name% (Device Type %dt%)...

REM Modify DEVICE_TYPE in sketch
powershell -Command "(Get-Content 'blinky-things.ino') -replace '#define DEVICE_TYPE \d+', '#define DEVICE_TYPE %dt%' | Set-Content 'blinky-things.ino'" >nul 2>&1

REM Compile
arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense . >nul 2>&1

if errorlevel 1 (
    echo    ❌ COMPILATION FAILED
    set TEST_PASSED=false
    
    REM Show compilation errors
    echo    Compilation errors:
    arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense .
) else (
    echo    ✅ COMPILATION PASSED
)

goto :eof