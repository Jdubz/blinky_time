# Safety Mechanisms - Multi-Layer Defense

This document describes the comprehensive safety system implemented to prevent device bricking and data corruption.

## 🎯 Defense Layers

### Layer 1: Documentation & Context (Persistent)

**`.clinerules`** - Persistent warnings for AI assistants
- Critical upload method restrictions
- Config version management rules
- Pre-upload checklist

**`DEVELOPMENT.md`** - Comprehensive developer guide
- Detailed safety procedures
- Common pitfalls and solutions
- Recovery instructions

**`SAFETY.md`** (this file) - Overview of all safety mechanisms

### Layer 2: Compile-Time Checks (Prevent Bad Code)

**Static Assertions** - `ConfigStorage.h:57-60`
```cpp
static_assert(sizeof(StoredMicParams) == 34,
    "StoredMicParams size changed! Increment CONFIG_VERSION and update assertion.");
```

**What it catches:**
- Struct size changes without CONFIG_VERSION increment
- Accidental field additions/removals
- Platform-specific padding issues

**Result:** Compilation FAILS if struct changes without version bump

### Layer 3: Runtime Validation (Detect Corrupt Data)

**Parameter Validation** - `ConfigStorage.cpp:173-177`
```cpp
validateFloat(data_.mic.baselineAttackTau, 0.01f, 1.0f, F("baselineAttackTau"));
validateFloat(data_.mic.baselineReleaseTau, 0.5f, 10.0f, F("baselineReleaseTau"));
// ... all parameters validated
```

**What it catches:**
- Garbage data from flash
- Out-of-range values
- Corrupt config files

**Result:** Automatically loads defaults if corruption detected

**Struct Size Logging** - `ConfigStorage.cpp:25-27`
```cpp
Serial.print(F("[CONFIG] ConfigData size: ")); Serial.print(sizeof(ConfigData));
Serial.print(F(" bytes (StoredMicParams: ")); Serial.print(sizeof(StoredMicParams));
Serial.println(F(" bytes)"));
```

**Purpose:** Helps debug padding/alignment issues

### Layer 4: Flash Safety (Prevent Bootloader Corruption)

**Flash Address Validation** - `SafetyTest.h:47-58`
```cpp
inline bool isFlashAddressSafe(uint32_t addr, uint32_t size = 4096) {
    if (addr < BOOTLOADER_END) return false;  // Protect bootloader
    if (addr + size > FLASH_END) return false;  // Prevent overflow
    if (addr % 4096 != 0) return false;        // Sector alignment
    return true;
}
```

**What it prevents:**
- Writing to bootloader region (< 0x30000)
- Flash overflow
- Unaligned sector writes

**Result:** System HALTS if unsafe write attempted

**Critical Safety Check** - `ConfigStorage.cpp:31-39`
```cpp
if (!SafetyTest::isFlashAddressSafe(flashAddr, 4096)) {
    Serial.println(F("[CONFIG] !!! UNSAFE FLASH ADDRESS DETECTED !!!"));
    // ... detailed error message ...
    flashOk = false;  // Disable ALL flash operations
}
```

**Result:** Flash operations completely disabled if address unsafe

### Layer 5: Pre-Upload Automation (Catch Mistakes Early)

**Safety Check Script** - `scripts/check_config_safety.py`

**What it checks:**
1. ✓ CONFIG_VERSION has descriptive comment
2. ✓ Static assertions present for struct sizes
3. ✓ All parameters have validation
4. ✓ Git status (uncommitted changes warning)

**Usage:**
```bash
python scripts/check_config_safety.py
```

**Result:** Blocks upload if critical issues found

**Git Pre-Commit Hook** - `.git/hooks/pre-commit`

**What it does:**
- Automatically runs safety script if ConfigStorage changed
- Reminds about CONFIG_VERSION increment
- Displays arduino-cli warning

**Result:** Can't commit dangerous code without explicit bypass

### Layer 6: Upload Method Enforcement (Prevent Bricking)

**Multiple Warnings:**
1. `.clinerules` - AI assistant sees EVERY session
2. Git pre-commit hook - Developer sees EVERY commit
3. DEVELOPMENT.md - Documented in 3 places
4. Safety script output - Displayed on EVERY check

**Policy:** NEVER use `arduino-cli upload` or `adafruit-nrfutil dfu serial`

### Layer 6b: Safe UF2 Upload (Linux/Headless)

**UF2 Upload Script** - `tools/uf2_upload.py`

The UF2 upload workflow provides safe CLI-based firmware upload by
using the board's UF2 mass storage bootloader instead of the fragile
`adafruit-nrfutil` DFU serial protocol.

**Upload workflow:**
1. Validates hex file using `pre_upload_check.py` (address safety)
2. Converts hex to UF2 format (family `0xADA52840`)
3. Enters bootloader via 1200 baud serial touch
4. Mounts UF2 mass storage drive (udisksctl)
5. Copies `firmware.uf2` to drive
6. Verifies reboot (drive disappears, serial port returns)

**Usage:**
```bash
make uf2-upload UPLOAD_PORT=/dev/ttyACM0   # Full workflow
make uf2-check UPLOAD_PORT=/dev/ttyACM0    # Dry run (no upload)
make uf2-test                               # Verify infrastructure
```

**Why UF2 is safer than DFU serial:**
- UF2 bootloader validates files before writing (cannot brick)
- Simple file copy (no serial protocol race conditions)
- Bootloader region is hardware-protected
- Interrupted transfers leave old firmware intact
- Invalid/corrupt files are silently rejected

**Policy update:** UF2 upload via `tools/uf2_upload.py` is the approved
CLI upload method. `arduino-cli upload` and `adafruit-nrfutil` remain BANNED.

---

## 🔄 Safety Workflow

### Normal Development Flow

```
1. Code changes
   ↓
2. Git commit attempt
   ↓
3. Pre-commit hook runs
   ├─→ Safety checks run
   ├─→ Warns if ConfigStorage changed
   └─→ Displays upload method warning
   ↓
4. Compile in Arduino IDE
   ├─→ Static assertions verify struct sizes
   └─→ Compilation fails if version mismatch
   ↓
5. Upload via Arduino IDE (ONLY!)
   ↓
6. Device boots
   ├─→ Runtime struct size logging
   ├─→ Flash address validation
   ├─→ Parameter validation
   └─→ Loads defaults if corruption detected
```

### Emergency: If Device Crashes

```
1. Check Serial output for error messages
   ↓
2. Is it a config issue?
   ├─ YES → Use serial console "factoryreset" command
   └─ NO  → Continue
   ↓
3. Can device boot at all?
   ├─ YES → Reflash with Arduino IDE
   └─ NO  → SWD recovery required (see DEVELOPMENT.md)
```

---

## 📊 Safety Mechanism Coverage

| Risk | Prevention | Detection | Recovery |
|------|-----------|-----------|----------|
| **arduino-cli upload** | .clinerules, docs, hooks | N/A | SWD hardware |
| **DFU serial upload** | UF2 upload script, docs | uf2_upload.py validation | Old firmware intact |
| **CONFIG_VERSION not incremented** | Static assert, safety script | Compile error | Fix code, recompile |
| **Missing validation** | Safety script | Runtime corruption | Auto-load defaults |
| **Corrupt flash data** | Validation checks | Runtime checks | Auto-load defaults |
| **Bootloader corruption** | Flash address validation | Runtime halt | SWD hardware |
| **Memory overflow** | SafetyTest bounds checks | Runtime detection | System halt |
| **Static init fiasco** | (Future: static analysis) | Early crash | Code review |

---

## 🎓 Lessons Learned

### What Went Wrong (This Incident)

1. **Arduino-cli used despite warning** → Device bricked
2. **CONFIG_VERSION not incremented** → Would crash even with correct upload
3. **Missing parameter validation** → Could read garbage data

### Root Cause

**Insufficient safety automation** - relied on manual memory

### Solution

**Multi-layer defense** - Make it HARD to make mistakes:
- Can't compile without version increment (static assert)
- Can't commit without safety check (pre-commit hook)
- Can't load corrupt data (runtime validation)
- Can't use arduino-cli without explicit bypass (docs everywhere)

---

## 🚀 Future Enhancements

Potential additional safety mechanisms:

1. **Static Initialization Analysis**
   - Script to detect dangerous global constructors
   - Check for initialization order dependencies
   - Mentioned in SafetyTest.h but not yet implemented

2. **Automated Testing in CI/CD**
   - Run safety checks on every PR
   - Compile for all supported platforms
   - Block merge if checks fail

3. **Firmware Versioning**
   - Add firmware version number
   - Log version on boot
   - Track version history in flash

4. **Safe Mode / Recovery Mode**
   - Hold button during boot to skip user code
   - Always boots to serial console
   - Can reflash even if app crashes

5. **Backup Configuration**
   - Store two config copies in flash
   - Fall back to backup if primary corrupt
   - CRC validation of config data

6. **Upload Wrapper Script**
   - Shell script that enforces Arduino IDE usage
   - Blocks arduino-cli completely
   - Runs safety checks automatically

---

## ✅ Quick Reference

**Before EVERY upload:**
```bash
python scripts/check_config_safety.py  # MUST PASS
# Then upload using Arduino IDE ONLY
```

**If you change config structs:**
1. Increment CONFIG_VERSION
2. Update static_assert
3. Add validation for new params
4. Update defaults, load, save
5. Run safety script

**If device crashes:**
1. Check serial output
2. Try factory reset via serial
3. Reflash via Arduino IDE
4. If bricked: SWD recovery (see DEVELOPMENT.md)

**NEVER:**
- ❌ Use arduino-cli for upload
- ❌ Change struct size without version increment
- ❌ Add parameters without validation
- ❌ Skip safety checks before upload

---

## 📞 Help & Resources

- **Development Guide**: DEVELOPMENT.md
- **Architecture**: blinky-things/README.md
- **Safety Tests**: blinky-things/tests/SafetyTest.h
- **Recovery Guide**: https://www.pavelp.cz/posts/eng-xiao-nrf52840-bootloader-recovery/
