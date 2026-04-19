# Development Guide - Blinky Time

## 🚨 Critical Safety Rules

### Upload Method (nRF52840)

**NEVER use `arduino-cli upload` directly for the nRF52840.**

`arduino-cli upload` uses the DFU serial protocol which can corrupt the bootloader.

**Safe upload options:**
1. **UF2 via script** (Linux/headless): `make uf2-upload UPLOAD_PORT=/dev/ttyACM0`
2. **UF2 manual drag-and-drop**: double-tap Reset → drag `.uf2` to `XIAO-SENSE` drive
3. **Arduino IDE**: Sketch → Upload (uses UF2 internally)

### Upload Method (ESP32-S3) — DEPRECATED

> **ESP32-S3 support was cut in March 2026.** All active development targets nRF52840 only. Below retained for historical reference.

The ESP32-S3 does not have the nRF52840 bootloader fragility. Both UF2 upload and `arduino-cli upload` work safely.

**Recommended:** `make esp32-uf2-upload UPLOAD_PORT=/dev/ttyACM0`

### Recovery (nRF52840 bricked by DFU upload)

If the bootloader was corrupted:
1. Requires external SWD programmer (Raspberry Pi Pico ~$5, or J-Link $15-50)
2. Requires Seeed XIAO expansion board OR direct soldering to SWD pads
3. Recovery process: https://www.pavelp.cz/posts/eng-xiao-nrf52840-bootloader-recovery/

---

## 📋 Pre-Upload Checklist

**Run BEFORE EVERY firmware upload:**

```bash
# 1. Run safety checks
python scripts/check_config_safety.py

# 2. Verify all tests pass (if applicable)
# Run in Arduino IDE with ENABLE_TESTING defined

# 3. Build in Arduino IDE (NOT arduino-cli)
# Verify -> Compile

# 4. Upload using Arduino IDE
# Upload -> Upload
```

---

---

## 🔲 Multi-Platform Development

The firmware supports XIAO nRF52840 Sense and XIAO ESP32-S3 Sense from a single codebase.

### Platform Detection

All platform-specific branching flows through `hal/PlatformDetect.h`:

```cpp
#if defined(ESP32)
  #define BLINKY_PLATFORM_ESP32S3  1
#elif defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_NRF52) || \
      defined(NRF52) || defined(NRF52840_XXAA)
  #define BLINKY_PLATFORM_NRF52840 1
#else
  #error "Unsupported platform. Add detection to hal/PlatformDetect.h"
#endif
```

**Rules:**
- Never use raw `#ifdef ESP32` or `#ifdef NRF52` outside `PlatformDetect.h`
- Always use `BLINKY_PLATFORM_*` macros in application code
- Add new platforms to `PlatformDetect.h` first, then implement HAL

### Platform Detection Macros Reference

| Core | Key Macros Defined |
|------|--------------------|
| Seeeduino nRF52 (non-mbed) | `NRF52840_XXAA`, `NRF52` |
| Seeeduino mbed | `ARDUINO_ARCH_MBED`, `ARDUINO_ARCH_NRF52840` |
| arduino-esp32 | `ESP32`, `ARDUINO_ARCH_ESP32` |
| SAMD (Arduino Zero etc.) | `ARDUINO_ARCH_SAMD` |
| AVR | `__AVR__`, `ARDUINO_ARCH_AVR` |

Note: `ARDUINO_ARCH_*` macros come from `platform.txt` as `-DARDUINO_ARCH_{build.arch}`. They are only present when that core is installed and selected.

### Adding Platform-Specific Code

**Preferred pattern — HAL interface + per-platform implementation:**

```
hal/
├── interfaces/IPdmMic.h        ← abstract interface (no platform code)
├── hardware/Nrf52PdmMic.h/cpp  ← nRF52840 implementation
├── hardware/Esp32PdmMic.h/cpp  ← ESP32-S3 implementation
└── DefaultHal.h                ← factory: returns correct impl at compile time
```

The `DefaultHal.h` factory pattern:
```cpp
#ifdef BLINKY_PLATFORM_NRF52840
  inline Nrf52PdmMic& pdm() { static Nrf52PdmMic i; return i; }
#elif defined(BLINKY_PLATFORM_ESP32S3)
  inline Esp32PdmMic& pdm() { static Esp32PdmMic i; return i; }
#endif
```

**Guard .cpp files at the file level:**
```cpp
// Nrf52PdmMic.cpp
#include "hal/PlatformDetect.h"
#ifdef BLINKY_PLATFORM_NRF52840
#include <PDM.h>
// ... all nRF52-specific implementation ...
#endif
```

This prevents the ESP32 toolchain from seeing `PDM.h` (which doesn't exist on ESP32).

### Key Platform Differences

| Feature | nRF52840 | ESP32-S3 |
|---------|----------|----------|
| PDM mic | Hardware PDM interrupt → `onReceive()` callback | I2S peripheral, polling via `poll()` in `AdaptiveMic::update()` |
| Flash storage | `mbed::FlashIAP` / `InternalFileSystem` | `Preferences` (NVS key-value store) |
| Battery monitor | GPIO + ADC divider circuit | Not available — all pins set to -1 |
| Compile output | `.hex` → convert to `.uf2` via `uf2conv.py` | `.bin` → convert to `.uf2` via `uf2conv.py -b 0x10000` |
| UF2 family ID | `0xADA52840` | `0xc47e5767` |

### Common Pitfalls

**Library not available on target platform**
Symptom: `fatal error: PDM.h: No such file or directory` when compiling for ESP32.
Fix: Guard the `#include` and the `.cpp` file with `#ifdef BLINKY_PLATFORM_NRF52840`.

**Library .cpp files cannot be gated from the sketch**
Arduino compiles library `.cpp` files independently — sketch-level `#define` values do not propagate. Always put platform guards inside the library `.cpp` file itself (or in its header).

**Struct layout differs across platforms**
The nRF52 (ARM Cortex-M4) and ESP32-S3 (Xtensa LX7) compilers may pad structs differently. `ConfigStorage` stores structs to flash — if struct layout diverges between platforms, the stored data becomes unreadable. Mitigations:
- Use `__attribute__((packed))` on stored structs
- Use fixed-width types (`uint32_t`, not `int`)
- Validate struct size with `static_assert` (will fail at compile time if padding changed)
- Each platform has a separate NVS namespace / flash region

**`ARDUINO_ARCH_NRF52` vs `NRF52840_XXAA`**
The mbed and non-mbed Seeeduino nRF52 cores define different macros. `PlatformDetect.h` handles both. Do not add new detection logic outside that file.

### Compiling for Both Platforms

```bash
# nRF52840
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" \
  --output-dir /tmp/blinky-nrf52 blinky-things/

# ESP32-S3
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3" \
  --output-dir /tmp/blinky-esp32 blinky-things/
```

Or via Makefile: `make compile-out` and `make esp32-compile`.

---

## ⚙️ Config Version Management

### When to Increment CONFIG_VERSION

**ALWAYS increment** when you:
- Add new fields to `StoredFireParams` or `StoredMicParams`
- Remove fields from config structs
- Change field types (e.g., `float` → `double`)
- Reorder struct fields

### How to Increment CONFIG_VERSION

**File: `blinky-things/config/ConfigStorage.h`**

```cpp
// Before:
static const uint8_t CONFIG_VERSION = 18;

// After:
static const uint8_t CONFIG_VERSION = 19;  // v19: describe your changes here
```

### Required Steps After Struct Changes

1. **Increment CONFIG_VERSION** in ConfigStorage.h
2. **Update comment** with description of changes
3. **Add validation** for new parameters in ConfigStorage.cpp `loadConfiguration()`
4. **Update static_assert** for struct size in ConfigStorage.h
5. **Update defaults** in `loadDefaults()`
6. **Update load/save** in `loadConfiguration()` and `saveConfiguration()`
7. **Run safety script**: `python scripts/check_config_safety.py`

### Example: Adding a New Parameter

```cpp
// 1. Add to struct (ConfigStorage.h)
struct StoredMicParams {
    // ... existing fields ...
    float newParameter;  // NEW: describe purpose
};

// 2. Increment version
static const uint8_t CONFIG_VERSION = 19;  // v19: added newParameter for XYZ

// 3. Update static_assert
static_assert(sizeof(StoredMicParams) == 38,  // Updated from 34
    "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion.");

// 4. Add default (ConfigStorage.cpp loadDefaults())
data_.mic.newParameter = 1.0f;  // Reasonable default

// 5. Add validation (ConfigStorage.cpp loadConfiguration())
validateFloat(data_.mic.newParameter, 0.0f, 10.0f, F("newParameter"));

// 6. Add to load (ConfigStorage.cpp loadConfiguration())
mic.newParameter = data_.mic.newParameter;

// 7. Add to save (ConfigStorage.cpp saveConfiguration())
data_.mic.newParameter = mic.newParameter;
```

---

## 🛡️ Safety Mechanisms

### Compile-Time Checks

**Static Assertions** (ConfigStorage.h):
```cpp
static_assert(sizeof(StoredMicParams) == 34,
    "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion.");
```

**Purpose**: Catches struct size changes at compile time

**If this fails**: You forgot to increment CONFIG_VERSION or update the assertion

### Runtime Checks

**Struct Size Logging** (prints at boot):
```
[CONFIG] ConfigData size: 72 bytes (StoredMicParams: 34 bytes)
```

**Purpose**: Helps debug padding issues across platforms

**Parameter Validation** (ConfigStorage.cpp):
```cpp
validateFloat(data_.mic.onsetThreshold, 1.5f, 5.0f, F("onsetThreshold"));
```

**Purpose**: Prevents loading corrupt/garbage data from flash

### Safety Tests

**Flash Address Validation** (SafetyTest.h):
- Prevents writing to bootloader region
- Validates all flash operations before execution

**Heap/Stack Tests**:
- Validates memory allocation works correctly
- Detects corruption early

---

## 🧪 Testing Best Practices

### Test Before Upload

Always test critical changes:
```cpp
#define ENABLE_TESTING
#include "BlinkyArchitecture.h"

void setup() {
    Serial.begin(115200);
    SafetyTest::runAllTests();  // MUST PASS
}
```

### Use Serial Console for Tuning

Prefer runtime tuning over reflashing:
```
show onsetthresh    # Check current value
set onsetthresh 3.0 # Modify value
save                # Persist to flash
```

---

## 🔍 Common Pitfalls

### ❌ Forgot to Increment CONFIG_VERSION

**Symptom**: Device crashes in boot loop after upload

**Cause**: Old flash data read with new struct layout

**Fix**: Increment CONFIG_VERSION, add validation, re-upload

### ❌ Missing Parameter Validation

**Symptom**: Random crashes or strange behavior

**Cause**: Garbage data from flash read into unvalidated parameter

**Fix**: Add `validateFloat()` or `validateUint32()` call

### ❌ Struct Padding Changed

**Symptom**: Config loads incorrectly across platforms

**Cause**: Compiler padding differs (32-bit vs 64-bit, different compilers)

**Fix**: Order fields by size (floats, uint16_t, uint8_t), use `#pragma pack` if needed

### ❌ Used `arduino-cli upload` on nRF52840

**Symptom**: Device bricked, reset button does nothing

**Cause**: DFU serial protocol corrupted bootloader

**Fix**: SWD recovery (see Recovery Requirements above). Use `make uf2-upload` instead.

---

## 📝 Code Review Checklist

Before merging config changes:

- [ ] CONFIG_VERSION incremented if struct changed
- [ ] Version comment describes changes
- [ ] Static assertions updated
- [ ] New parameters have validation
- [ ] Defaults set in `loadDefaults()`
- [ ] Load/save methods updated
- [ ] Safety script passes: `python scripts/check_config_safety.py`
- [ ] Compiled for **both** platforms: `make compile-out` and `make esp32-compile`
- [ ] No `arduino-cli upload` usage on nRF52840 in commit history

Before merging platform-specific code:

- [ ] Platform detection goes through `hal/PlatformDetect.h` — no raw `#ifdef ESP32` in app code
- [ ] Platform `.cpp` files have file-level `#ifdef BLINKY_PLATFORM_*` guards
- [ ] New HAL interfaces have a default no-op or stub for platforms that don't need them
- [ ] Struct sizes verified consistent across platforms (or documented as platform-local)

---

## 🌐 Console + Server Development

The `blinky-console` (React/Vite SPA) and `blinky-server` (FastAPI fleet
manager) can run independently or together.

### Standalone console (WebSerial only)

```bash
cd blinky-console && npm run dev
```

Opens at `http://localhost:3000`. Talks WebSerial directly to plugged-in
devices. No server needed.

### Console + server together

```bash
# Terminal 1: server (port 8420)
cd blinky-server && ./venv/bin/python -m blinky_server

# Terminal 2: console (port 3000, proxies /api and /ws to :8420)
cd blinky-console && npm run dev
```

Console at `:3000` keeps HMR + WebSerial; `/api/*` and `/ws/*` requests
proxy through to the server. Edit either side without rebuilding.

### Production-style: server serves the SPA

```bash
cd blinky-console && npm run build       # outputs to ../blinky-server/web/
cd blinky-server && ./venv/bin/python -m blinky_server
```

Server at `http://localhost:8420` serves both the API and the SPA from
the same origin. Override the static dir location with the
`BLINKY_STATIC_DIR` env var if needed.

---

## 🔗 Additional Resources

- [SafetyTest Documentation](blinky-things/tests/SafetyTest.h)
- [SerialConsole Commands](blinky-things/inputs/SerialConsole.cpp)
- [Architecture Overview](../blinky-things/README.md)
- [XIAO nRF52840 Recovery Guide](https://www.pavelp.cz/posts/eng-xiao-nrf52840-bootloader-recovery/)

---

## 💡 Pro Tips

1. **Use git branches** for risky changes
2. **Test config changes** with multiple power cycles
3. **Keep a backup device** for development
4. **Document WHY**, not just what, in comments
5. **Serial logging** is your friend - add generous debug output
6. **Factory reset** via serial console if config gets corrupted
