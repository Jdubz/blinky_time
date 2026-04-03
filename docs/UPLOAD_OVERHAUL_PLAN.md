# Upload Overhaul Plan

*Created: April 3, 2026 — Completed: April 3, 2026*

## Problem (Resolved)

Software-initiated bootloader entry via GPREGRET did not work on our Seeed XIAO nRF52840 devices. The register was set correctly (verified by readback) but cleared before the Adafruit bootloader could read it after NVIC_SystemReset(). Root cause: VIA Labs USB hub (2109:2813) power-cycles ports during device reset, causing a power-on reset that clears GPREGRET.

## Solution: Custom Bootloader with RAM-Based Entry

Replace the Adafruit bootloader's GPREGRET check with a **retained RAM flag** at a known address. nRF52840 RAM is preserved across system resets (AIRCR.SYSRESETREQ) — the same class of reset that GPREGRET is supposed to survive. But RAM is a larger, more reliable target with no peripheral-level clearing behavior.

### How It Works

**Firmware side (before reset):**
```c
// Write magic to a known RAM address INSTEAD of GPREGRET
#define BOOTLOADER_MAGIC_ADDR  ((volatile uint32_t*)0x20007F7C)  // Same as Adafruit's DFU_DBL_RESET_MEM
#define BOOTLOADER_MAGIC_UF2   0xBEEF0057
#define BOOTLOADER_MAGIC_BLE   0xBEEF00A8

*BOOTLOADER_MAGIC_ADDR = BOOTLOADER_MAGIC_UF2;
__DSB(); __ISB();
NVIC_SystemReset();
```

**Bootloader side (on boot):**
```c
// Check RAM flag before checking GPREGRET
uint32_t ram_magic = *((volatile uint32_t*)0x20007F7C);
if (ram_magic == BOOTLOADER_MAGIC_UF2) {
    *((volatile uint32_t*)0x20007F7C) = 0;  // Clear flag
    // Enter UF2 mode
    uf2_dfu = true;
    dfu_start = true;
} else if (ram_magic == BOOTLOADER_MAGIC_BLE) {
    *((volatile uint32_t*)0x20007F7C) = 0;
    // Enter BLE DFU mode
    _ota_dfu = true;
    dfu_start = true;
} else {
    // Fall through to existing GPREGRET + double-reset detection
    // (preserves backward compatibility)
}
```

### Why This Works

1. **RAM at 0x20007F7C survives system reset** — this is the SAME address the Adafruit bootloader already uses for double-reset detection. If the bootloader can read the double-reset magic from RAM, it can read our magic from the same location.

2. **No GPREGRET dependency** — bypasses whatever is clearing the register. The RAM write is a simple 32-bit store, no SoftDevice API needed, no peripheral interaction.

3. **Backward compatible** — the bootloader still checks GPREGRET and double-reset as fallback. Old firmware continues to work.

4. **Same address as double-reset detection** — if double-reset works (and it does via physical button), then RAM at this address provably survives resets on these devices.

### Why Not Just Use the Double-Reset RAM?

The Adafruit bootloader's double-reset detection writes `DFU_DBL_RESET_MAGIC = 0x5A1AD5` to `0x20007F7C`, waits 500ms, then clears it. If a second reset arrives within 500ms, the magic is still there.

We could exploit this by writing `0x5A1AD5` to that address and resetting — the bootloader would think it's the "second tap" of a double-reset. But the bootloader also checks `POWER_RESETREAS_RESETPIN_Msk` (pin reset reason). A soft reset (NVIC_SystemReset) sets a different reset reason bit. So the double-reset detection specifically requires PIN resets, not software resets.

The custom bootloader modification removes this pin-reset requirement for our magic values, while preserving it for the actual physical double-tap.

## Implementation (Completed April 3, 2026)

All phases completed. Custom bootloader deployed to all 4 blinkyhost devices via UF2 self-update (no SWD needed). Firmware updated to use RAM magic on all bootloader entry paths.

**Bootloader changes** (`Adafruit_nRF52_Bootloader/src/main.c`):
- Added `DFU_RAM_MAGIC_UF2` (0xBEEF0057), `DFU_RAM_MAGIC_BLE` (0xBEEF00A8), `DFU_RAM_MAGIC_QSPI` (0xBEEF00CC)
- `check_dfu_mode()` checks RAM at `DFU_DBL_RESET_MEM` before GPREGRET
- QSPI apply also checks RAM magic before GPREGRET
- Falls through to stock GPREGRET + double-reset if no RAM magic (backward compatible)

**Firmware changes**:
- `Uf2BootloaderOverride.h` — writes `0xBEEF0057` to RAM (1200-baud touch)
- `SerialConsole.cpp` — writes `0xBEEF0057` or `0xBEEF00A8` to RAM (`bootloader` / `bootloader ble`)
- `SafeBootWatchdog.h` — writes RAM magic for recovery (UF2 or BLE DFU)
- `QspiOtaStaging.h` — writes `0xBEEF00CC` to RAM (`ota commit`)
- All paths: no `Serial.flush()` before reset (commands may arrive over BLE NUS)

## Bootloader Build Instructions

```bash
cd /home/jdubz/Development/Adafruit_nRF52_Bootloader

# Build for XIAO nRF52840 Sense
make BOARD=xiao_nrf52840_sense all

# Output: _build/build-xiao_nrf52840_sense/xiao_nrf52840_sense_bootloader-*.hex
```

**SWD Flash via Pi GPIO:**
```bash
# On blinkyhost, connect:
#   Pi GPIO24 → SWDIO pad on nRF52840
#   Pi GPIO25 → SWDCLK pad on nRF52840
#   Pi GND    → GND pad on nRF52840

openocd -f interface/bcm2835gpio-swd.cfg -f target/nrf52.cfg \
  -c "init; reset halt; program bootloader.hex verify; reset; exit"
```

## Risk Assessment

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Custom bootloader has bugs | Medium | Test extensively on bare chip before fleet |
| RAM address collision with app | Low | 0x20007F7C is already reserved by bootloader |
| SWD damages device | Very Low | Proven procedure, only writes to flash |
| New bootloader breaks BLE DFU | Medium | Test BLE DFU entry before fleet deployment |
| Power-on reset clears RAM flag | Expected | Only affects cold boot, not soft reset. Firmware re-writes flag before every reset. |

## Alternative Approaches Considered

### Direct Bootloader Jump (Rejected)
Jump directly to bootloader address (0x1000) from firmware without NVIC_SystemReset(). Risks: wrong stack pointer, uninitialized peripherals, no clean USB re-enumeration. Too dangerous for production.

### USB Hub Replacement (Impractical)
Replace VIA Labs hub with one that doesn't power-cycle. Requires physical access and hardware changes.

### Serial DFU Only (Rejected)
Use `adafruit-nrfutil dfu serial` exclusively. Fragile protocol with documented race conditions. Has bricked devices before.

### Always-Enter-Bootloader Timeout (Considered)
Bootloader waits 2-3 seconds on every boot before jumping to app. Adds latency to every power-on. Acceptable if other approaches fail.
