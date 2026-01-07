@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if not exist build mkdir build
cl /EHsc /std:c++17 /O2 /nologo /Fe:build\blinky-simulator.exe /I include /I ..\blinky-things src\main.cpp ..\blinky-things\types\PixelMatrix.cpp ..\blinky-things\math\SimplexNoise.cpp ..\blinky-things\generators\Fire.cpp ..\blinky-things\generators\Water.cpp ..\blinky-things\generators\Lightning.cpp ..\blinky-things\effects\HueRotationEffect.cpp ..\blinky-things\render\EffectRenderer.cpp ..\blinky-things\render\RenderPipeline.cpp /link /OUT:build\blinky-simulator.exe
