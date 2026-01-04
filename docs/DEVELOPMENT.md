# Development Guide - Blinky Time

## 🚨 Critical Safety Rules

### Upload Method - NON-NEGOTIABLE

**NEVER use `arduino-cli` for uploading firmware!**

- `arduino-cli upload` is fundamentally flawed and WILL brick the device
- Bricked devices require SWD hardware recovery (J-Link, Raspberry Pi Pico as debugger)
- **ONLY** upload using the **Arduino IDE GUI**

### Why arduino-cli Fails

The arduino-cli tool has known issues with nRF52840 bootloader interaction:
- Corrupts bootloader during upload process
- Leaves device in unrecoverable state without hardware intervention
- Reset button will NOT engage bootloader after corruption
- Safe mode does NOT activate

### Recovery Requirements

If device is bricked by arduino-cli:
1. Requires external SWD programmer (Raspberry Pi Pico ~$5, or J-Link $15-50)
2. Requires Seeed XIAO expansion board OR direct soldering to SWD pads
3. Recovery process documented at: https://www.pavelp.cz/posts/eng-xiao-nrf52840-bootloader-recovery/

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

### ❌ Used arduino-cli for Upload

**Symptom**: Device bricked, reset button does nothing

**Cause**: arduino-cli corrupted bootloader

**Fix**: SWD recovery (see Recovery Requirements above)

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
- [ ] Compiled successfully in Arduino IDE
- [ ] No arduino-cli usage in commit history

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
