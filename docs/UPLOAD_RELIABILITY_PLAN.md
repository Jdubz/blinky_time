# Upload Reliability Plan

*Created: April 3, 2026*

## Problem

Firmware upload via blinky-server fails intermittently on blinkyhost because the VIA Labs USB hub (2109:2813) power-cycles ports during device reset, clearing GPREGRET before the bootloader can read it. This affects both UF2 and BLE DFU bootloader entry.

## Root Cause

1. Firmware sets GPREGRET=0x57 (UF2) or 0xA8 (BLE DFU) via SoftDevice API
2. Firmware calls NVIC_SystemReset()
3. Device USB disconnects during reset
4. Hub detects disconnect and briefly cuts port power (per-port power switching)
5. Power loss causes power-on reset instead of system reset
6. GPREGRET is cleared (Nordic spec: "retained through system reset, cleared on power-on reset")
7. Bootloader sees GPREGRET=0x00 and boots application instead of entering bootloader mode

## Fix: Lock USB port power during bootloader entry

Before sending the bootloader command, use `uhubctl` to explicitly lock port power ON. This prevents the hub from power-cycling during the USB disconnect/reconnect that occurs during NVIC_SystemReset().

### Sequence (UF2 path):

1. Determine device's USB hub port via `udevadm` or sysfs
2. `uhubctl -l <hub> -p <port> -a on` — lock power ON
3. Send `bootloader\r\n` via serial
4. Wait for UF2 mass storage to appear (8-15s)
5. Copy firmware.uf2 to UF2 drive
6. Device reboots into new firmware
7. Port power remains locked ON throughout

### Sequence (BLE DFU path):

1. Same hub port power lock
2. Send `bootloader ble\r\n` via serial
3. Wait for BLE DFU service to advertise
4. Transfer firmware via BLE DFU protocol
5. Device reboots into new firmware

### Implementation

**In `tools/uf2_upload.py`:**
- Add `_lock_hub_power(hub_path, hub_port)` before `trigger_bootloader()`
- The hub path and port are already detected (lines 880-893)
- Requires `sudo uhubctl` — add to sudoers for the blinkytime user

**In `blinky-server/blinky_server/firmware/uf2_upload.py`:**
- Before spawning the subprocess, lock the hub port power
- Pass hub info to the subprocess if needed

**In `blinky-server/blinky_server/firmware/ble_dfu.py`:**
- Before sending `bootloader ble`, lock the hub port power
- Requires knowing the device's USB hub location

## Sudoers

Required in `/etc/sudoers.d/blinky` (narrow rules, no blanket python root):
```
blinkytime ALL=(ALL) NOPASSWD: /usr/bin/uhubctl
blinkytime ALL=(ALL) NOPASSWD: /usr/bin/python3 /home/blinkytime/blinky_time/tools/usb_reset.py
```

## TODO

- Hub port power locking via uhubctl during bootloader entry
- See also `UPLOAD_OVERHAUL_PLAN.md` for the long-term fix (custom bootloader with RAM-based entry)
