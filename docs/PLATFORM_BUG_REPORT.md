# Platform Bug Report: pinDefinitions.h Missing Include Guards

## Issue Summary
The Seeeduino mbed platform's `pinDefinitions.h` file lacks include guards, causing redefinition errors when multiple libraries include it.

## Affected Versions
- **Platform**: Seeeduino:mbed 2.9.2, 2.9.3 (confirmed)
- **File**: `cores/arduino/pinDefinitions.h`
- **Libraries Affected**: Adafruit_NeoPixel + PDM (and potentially others)

## Problem Description

### Root Cause
The file `pinDefinitions.h` in the Seeeduino mbed core does not have include guards:
```cpp
// Current file starts with:
#ifdef USE_ARDUINO_PINOUT
  // ... definitions ...
// (No #pragma once or include guards)
```

### Symptom
When both `Adafruit_NeoPixel` and `PDM` libraries are used together, compilation fails with:
```
error: redefinition of 'struct _PinDescription'
error: redefinition of 'struct _AnalogPinDescription'
error: redefinition of 'PinName digitalPinToPinName(pin_size_t)'
```

### Why It Happens
Both libraries include `pinDefinitions.h`:
- `Adafruit_NeoPixel.h:54` includes `pinDefinitions.h`
- `PDM.h:23` includes `<pinDefinitions.h>`

Without include guards, the file gets processed twice, causing redefinitions.

## Reproduction Steps

1. Create Arduino sketch for nRF52840 XIAO Sense
2. Include both libraries:
   ```cpp
   #include <Adafruit_NeoPixel.h>
   #include <PDM.h>
   ```
3. Compile with Seeeduino:mbed platform 2.9.2 or 2.9.3
4. Observe redefinition errors

## Impact

This bug prevents using:
- **Audio-reactive LED projects** (NeoPixel + PDM microphone)
- **Any project combining NeoPixel LEDs with PDM audio**

Our project (`blinky_time`) requires both for audio-reactive fire effects and is currently blocked.

## Proposed Fix

### Option 1: Add #pragma once (Recommended)
```cpp
#pragma once  // Add this line at the top

#ifdef USE_ARDUINO_PINOUT
  // ... rest of file ...
```

### Option 2: Traditional Include Guards
```cpp
#ifndef ARDUINO_PINDEFINITIONS_H
#define ARDUINO_PINDEFINITIONS_H

#ifdef USE_ARDUINO_PINOUT
  // ... rest of file ...
#endif

#endif // ARDUINO_PINDEFINITIONS_H
```

## Workarounds

### Current Workaround (Temporary)
We've temporarily disabled the PDM/AdaptiveMic functionality:
- Audio input disabled (fire effects run without music reactivity)
- Can be re-enabled once platform is fixed

### Historical Note
This code **DID compile successfully** a few weeks ago with Seeeduino:mbed 2.9.2, suggesting this may be a recent regression or the old working state was coincidental based on include order.

## Files to Report To

- **Seeeduino GitHub**: https://github.com/Seeed-Studio/ArduinoCore-mbed
- **File Location**: `cores/arduino/pinDefinitions.h`
- **Platforms Affected**: All nRF52840-based boards using mbed core

## Additional Context

### Our Project
- **Project**: Blinky Time LED Fire Effect Controller
- **Hardware**: nRF52840 XIAO Sense
- **Use Case**: Audio-reactive fire effects with NeoPixel LEDs
- **Repository**: https://github.com/Jdubz/blinky_time

### Testing Environment
- **Arduino CLI**: Latest
- **Platform**: Seeeduino:mbed 2.9.2, 2.9.3
- **Adafruit_NeoPixel**: 1.15.1
- **PDM**: 1.0 (bundled with platform)

## Priority
**HIGH** - This blocks a common use case (LEDs + audio) for the platform

---
*Report generated: 2025-01-05*
*For: blinky_time project - LED Fire Effect Controller*
