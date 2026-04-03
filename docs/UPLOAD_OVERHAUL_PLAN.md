# Upload Overhaul Plan

*Created: April 3, 2026*

## Problem

Software-initiated bootloader entry via GPREGRET does not work on our Seeed XIAO nRF52840 devices. The register is set correctly (verified by readback) but cleared before the Adafruit bootloader can read it after NVIC_SystemReset(). This affects ALL upload paths: serial command, 1200-baud touch, and BLE DFU entry. Physical double-tap reset works but requires physical access.

The root cause is not fully understood — USB hub power-cycling was disproved (uhubctl lock didn't help). The MBR, SoftDevice shutdown sequence, or a hardware interaction may be clearing the register.

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

## Implementation Plan

### Phase 1: Build and Test Custom Bootloader

**On devtop (local build):**
1. Fork the Adafruit bootloader source (already at `/home/jdubz/Development/Adafruit_nRF52_Bootloader/`)
2. Modify `src/main.c` to check RAM magic BEFORE GPREGRET check
3. Use magic values that are distinct from `DFU_DBL_RESET_MAGIC` (0x5A1AD5)
4. Build for `xiao_nrf52840_sense` target
5. Test on devtop with a locally connected bare chip (if available)

**On blinkyhost (SWD deployment to ONE test device):**
1. Connect SWD wires to ONE bare test chip (062CBD12 or 2A798EF8 — both have accessible pads)
2. Flash custom bootloader via OpenOCD + Pi GPIO
3. Flash application firmware via the new bootloader's UF2 mode
4. Verify: `bootloader` serial command → UF2 drive appears → firmware copy → reboot

**Only after single-device success:**
5. Flash bootloader to remaining bare chip
6. Flash bootloader to installed devices (Long Tube, etc.) via the now-working UF2 path from the already-updated device

### Phase 2: Update Firmware Bootloader Entry

1. Replace GPREGRET writes in Uf2BootloaderOverride.h with RAM magic writes
2. Replace GPREGRET writes in SerialConsole.cpp bootloader command
3. Replace GPREGRET writes in SafeBootWatchdog.h recovery
4. Keep GPREGRET writes as SECONDARY (fallback for stock bootloaders)

### Phase 3: Update Upload Tools

1. `tools/uf2_upload.py` — no changes needed (it sends `bootloader\r\n` via serial, firmware handles the rest)
2. `blinky-server/firmware/uf2_upload.py` — ensure streaming is stopped before upload (already done)
3. `blinky-server/firmware/__init__.py` — keep current no-fallback architecture

### Phase 4: Fleet Deployment

1. SWD flash custom bootloader to all devices (one at a time, verify each)
2. UF2 flash latest application firmware
3. Verify full upload pipeline: server API → UF2 → success on all devices
4. Run validation suite to confirm firmware functionality

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
