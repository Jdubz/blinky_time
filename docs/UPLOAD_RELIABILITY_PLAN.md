# Upload Reliability Plan

*Created: April 3, 2026 — Completed: April 3, 2026*

## Problem (Resolved)

Firmware upload via blinky-server failed intermittently because the VIA Labs USB hub (2109:2813) power-cycles ports during NVIC_SystemReset(), causing a power-on reset that clears GPREGRET before the bootloader can read it.

## Solution: Custom Bootloader with RAM-Based Entry

Replaced GPREGRET dependency with retained RAM flag at `0x20007F7C`. This is the same address the Adafruit bootloader uses for double-reset detection — proven to survive system resets on these devices.

**Custom bootloader** (Adafruit_nRF52_Bootloader fork) checks RAM magic values BEFORE GPREGRET:
- `0xBEEF0057` → UF2 mass storage mode
- `0xBEEF00A8` → BLE OTA DFU mode
- `0xBEEF00CC` → QSPI staged firmware apply

Falls through to stock GPREGRET + double-reset detection if no RAM magic found (backward compatible).

**All firmware upload entry points** now write RAM magic instead of GPREGRET:
- `Uf2BootloaderOverride.h` — 1200-baud touch (UF2)
- `SerialConsole.cpp` — `bootloader` / `bootloader ble` commands
- `SafeBootWatchdog.h` — crash recovery (UF2 or BLE DFU)
- `QspiOtaStaging.h` — `ota commit` (QSPI apply)

No `Serial.flush()` before any reset — commands may arrive over BLE NUS where there is no serial connection.

## Server Upload Pipeline

The server wrapper (`blinky-server/firmware/uf2_upload.py`) handles CDC state:
1. Captures USB bus/device path before transport disconnect
2. Disconnects transport
3. Sends `USBDEVFS_RESET` ioctl via `tools/usb_reset.py` to reinitialize TinyUSB CDC
4. Launches `tools/uf2_upload.py` with the correct port

## Sudoers

Required in `/etc/sudoers.d/blinky`:
```
blinkytime ALL=(ALL) NOPASSWD: /usr/bin/uhubctl
blinkytime ALL=(ALL) NOPASSWD: /usr/bin/python3 /home/blinkytime/blinky_time/tools/usb_reset.py
```
