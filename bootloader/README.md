# Modified Adafruit nRF52 Bootloader

## Purpose

Stock Adafruit bootloader enters **UF2 mass storage mode** when no valid
application exists (e.g., after a failed BLE DFU transfer mid-write). For
physically installed devices without USB access, this is unrecoverable.

This modified bootloader adds `DEFAULT_TO_OTA_DFU=1`, which changes the
fallback to **BLE DFU mode** when no valid app exists. The fleet server can
then retry the firmware upload wirelessly.

## Build Details

- **Source**: Adafruit_nRF52_Bootloader (upstream HEAD, v0.10.0)
- **Board**: `xiao_nrf52840_ble_sense`
- **Flag**: `DEFAULT_TO_OTA_DFU=1` (with OTAFIX-style logic — preserves explicit UF2/serial requests)
- **SoftDevice**: S140 v7.3.0 (unchanged, not included in update UF2)
- **Toolchain**: arm-none-eabi-gcc 9.2.1

## Files

| File | Purpose |
|------|---------|
| `update-bootloader-ota-default.uf2` | Self-update UF2 (flash via UF2 mass storage) |

## How to Flash

The update UF2 uses the MBR's power-fail-safe `SD_MBR_COMMAND_COPY_BL` to
atomically replace the bootloader. Safe to interrupt (MBR resumes copy on
next boot).

```bash
# Via uf2_upload.py (recommended — handles bootloader entry + UF2 copy)
python3 tools/uf2_upload.py /dev/ttyACM0 \
    --uf2 bootloader/update-bootloader-ota-default.uf2

# Via make target
make bootloader-update UPLOAD_PORT=/dev/ttyACM0
```

## Behavior Change

| Condition | Stock Bootloader | Modified Bootloader |
|-----------|-----------------|-------------------|
| No valid app, no GPREGRET | UF2 (USB) | **BLE DFU (wireless)** |
| GPREGRET=0x57 | UF2 | UF2 (preserved) |
| GPREGRET=0xA8 | BLE DFU | BLE DFU |
| Double-reset button | UF2 | UF2 (preserved) |
| Valid app | Boot app | Boot app |

Uses OTAFIX-style logic: only overrides to BLE DFU when no explicit
UF2/serial mode was requested. All wired recovery paths still work.

## Rebuilding

```bash
cd ~/Development/Adafruit_nRF52_Bootloader
export PATH="$HOME/.arduino15/packages/Seeeduino/tools/arm-none-eabi-gcc/9-2019q4/bin:$PATH"
pip3 install intelhex
make BOARD=xiao_nrf52840_ble_sense DEFAULT_TO_OTA_DFU=1 all
```

## Critical Fixes (applied to upstream v0.10.0)

### 1. OTAFIX-style DFU mode selection (`src/main.c`)

Upstream `DEFAULT_TO_OTA_DFU` overrides ALL DFU entry to BLE OTA, including
explicit UF2 requests (GPREGRET=0x57). This makes devices permanently
unreachable via UF2.

**Fix**: Only default to BLE OTA when no explicit mode is requested:
```c
if ((dfu_start || !valid_app) && !serial_only_dfu && !uf2_dfu) {
    _ota_dfu = 1;
}
```

### 2. BLE DFU timeout (`bootloader.c`)

Upstream `bootloader_dfu_start()` only starts the timeout timer for
serial/USB DFU. BLE DFU runs **forever** with no timeout. A device that
enters BLE DFU and receives no connection is permanently stuck until
power-cycled.

**Fix**: Start timeout timer for both BLE and serial paths.

### 3. No-valid-app re-entry (`src/main.c`)

When DFU completes but no valid app exists (failed mid-transfer), the
upstream bootloader resets with GPREGRET=0 → falls into UF2 mode.

**Fix**: Set GPREGRET=0xA8 (BLE DFU) when no valid app exists, so the
fleet server can retry wirelessly.

## Risk Assessment

- **MBR is never modified** (factory-burned at 0x00000000-0x00001000)
- **Self-update is power-fail-safe** (MBR tracks copy state, resumes after power loss)
- **VID/PID verified** before copy (prevents wrong-board bootloader)
- **If new bootloader is buggy**: device needs SWD/J-Link to recover (test on local device first!)
- **SoftDevice preserved** (update UF2 is `_nosd` — does not touch SoftDevice region)
