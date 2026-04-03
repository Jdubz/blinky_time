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

## Architecture Changes

### Remove fallback

`upload_with_ble_fallback()` in `firmware/__init__.py` should be replaced:

```python
async def upload_firmware(device, firmware_path):
    if device.state == DeviceState.DFU_RECOVERY:
        return await upload_ble_dfu(...)  # Already in bootloader
    
    if device.transport.transport_type == "serial":
        return await upload_uf2(...)  # UF2 only, no fallback
    
    if device.transport.transport_type == "ble":
        return await upload_ble_dfu(...)  # BLE DFU only
    
    return {"status": "error", "message": "No upload method"}
```

No fallback. If UF2 fails on a serial device, the error is returned immediately. The operator must investigate and fix the issue.

### Fleet flash stops on failure

`fleet_flash()` should stop on first device failure instead of continuing:

```python
for device in devices:
    result = await upload_firmware(device, firmware_path)
    if result["status"] != "ok":
        return {"status": "error", "failed_device": device.id, "result": result}
```

### Sudoers for uhubctl

Add to `/etc/sudoers.d/blinky`:
```
blinkytime ALL=(ALL) NOPASSWD: /usr/bin/uhubctl
```

## Validation

After implementing:
1. Test on ONE device first (bare chip with accessible reset as backup)
2. Verify GPREGRET readback shows 0x57 in response
3. Verify UF2 drive appears within 5s
4. Verify firmware copied and device reboots successfully
5. Only after single-device success, test fleet flash on all devices
