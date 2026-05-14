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

| File | Date | Features | Purpose |
|------|------|----------|---------|
| `restore-stock-bootloader-0.6.2.dfu.zip` | Mar 29 | none (stock) | Roll back to factory Seeed bootloader |
| `update-bootloader-0.8.0-ota-default.dfu.zip` | Mar 29 | DEFAULT_TO_OTA_DFU only | First OTA-default build (BLE DFU recovery, no QSPI) |
| `update-bootloader-ota-default.uf2` | Mar 29 | DEFAULT_TO_OTA_DFU only | Same as above, UF2 format for mass-storage flash |
| `update-bootloader-qspi-ota.uf2` | Mar 31 | RAM magic + QSPI staged OTA, **no** DEFAULT_TO_OTA_DFU | Previous production fleet bootloader |
| **`update-bootloader-qspi-ota-default.uf2`** | **May 13** | **RAM magic + QSPI + DEFAULT_TO_OTA_DFU + dual-transport DFU mode** | **Current target for sculpture installs (F1 in `docs/SCULPTURE_BLE_RECOVERY_PLAN.md`)** |
| `update-bootloader-qspi-ota-default_s140_7.3.0.zip` | May 13 | Same + SoftDevice | DFU.zip form, for OTA bootloader update via fleet server |

### Third critical fix in current build (`0.8.0-4-g76d1e60`): dual-transport DFU mode

Stock behavior gated which DFU transport was active on entry mode: a UF2/serial
DFU entry got USB MSC only, an OTA entry got BLE only. That meant a device's
recoverability depended on how it last entered DFU mode — fine for the wired
workflow, fatal once you commit to one bootloader image for both sealed and
USB-accessible devices.

The current build always brings up BOTH transports whenever the bootloader is in
DFU mode (entry-mode-agnostic):
- `check_dfu_mode()` always calls `ble_stack_init()` + `usb_init()`
- `bootloader_dfu_start()` always registers `dfu_transport_ble_update_start()`,
  regardless of the `ota` flag

Result: a single bootloader image works for every device. Whoever reaches the
device first — host PC over USB-MSC drop, or fleet server over BLE — wins.

Verified on hat (sn `C04C56F9DFC31D84`, 2026-05-13):
- `bootloader` serial command → both `/dev/sde` (USB MSC) and BLE `AdaDFU` advertising visible simultaneously ✓

End-to-end DFU completion via each transport on this build still owes a roll
across the bench fleet — track that separately, not as a property of this fix.

### Critical fix in prior build (`0.8.0-2-g6827504`)

The original `DEFAULT_TO_OTA_DFU` implementation forced BLE OTA mode for
*any* DFU entry without an explicit UF2/serial magic — including the
physical-user paths: **double-tap reset, DFU button, and
`APP_ASKS_FOR_SINGLE_TAP_RESET`**. That broke wired recovery on
USB-accessible devices: double-tapping reset went to BLE DFU instead of
USB UF2 mass-storage.

The current build excludes physical-user DFU entry from the
`DEFAULT_TO_OTA_DFU` coercion. F1 now fires only for "silent" triggers
(invalid app from mid-DFU corruption, app-initiated DFU without
explicit magic) — the actual sealed-sculpture scenario it was designed
for. Physical paths retain their traditional UF2 behavior.

Verified on bench device 2A798EF8 (2026-05-13):
- Double-tap reset → USB UF2 mode ✓
- `bootloader` serial command → USB UF2 mode ✓
- `bootloader ble` serial command → BLE DFU mode ✓
- 1200-baud touch (Arduino-style upload) → USB UF2 mode ✓

### Second critical fix in same build: SD-region write guard

The bootloader's `in_app_space()` originally used `USER_FLASH_START`
(= `MBR_SIZE`, 0x1000) as the lower bound. That range silently included
the SoftDevice (0x1000–`CODE_REGION_1_START`, e.g. 0x1000–0x27000 for
S140 v7.3.0). An app UF2 with blocks targeting addresses inside the SD
region — whether from a combined SD+app hex, OR a hand-crafted "kill"
UF2 — would silently erase SD pages and write 0xFFs back. The result
was a half-corrupted SoftDevice and a hard-to-diagnose WDT reset loop
(symptom: app crashes ~every 15 s, no hint that flash got scribbled on).

Fix:
- `in_app_space()` now uses `CODE_REGION_1_START` as the lower bound
  (runtime SD end, collapses to `MBR_SIZE` if no SD).
- Write-handler's "skip MBR" branch extended to cover the full pre-app
  region (`addr < CODE_REGION_1_START`) — matches the existing
  silent-skip semantics for combined SD+app hex UF2s.

Host-side: `tools/uf2_upload.py` now also validates `--uf2` files (this
path previously bypassed all safety checks). An app-family UF2 must
have at least one block targeting the app region and may not contain
blocks targeting the bootloader region; a bootloader-family UF2 must
target bootloader/UICR/MBR/SD only. Defense in depth.

### App start address (S140 v7.3.0)

If you ever need to craft a UF2 that targets the app region (e.g., for
a recovery test), the app start address is **`CODE_REGION_1_START`**,
which equals `SD_SIZE_GET(MBR_SIZE)` at runtime. For S140 v7.3.0 this
resolves to **0x27000**, NOT 0x26000. Addresses 0x1000–0x27000 are the
SoftDevice — writing 0xFFs there corrupts the running BLE stack.

The bench-device F1 destructive test that triggered the fixes above
made exactly this mistake: wrote to 0x26000 thinking it was the app
start. With the fix in place the bootloader silently ignores those
blocks (no harm done), and the host tool refuses the UF2 outright.

## How to Flash

The update UF2 uses the MBR's power-fail-safe `SD_MBR_COMMAND_COPY_BL` to
atomically replace the bootloader. Safe to interrupt (MBR resumes copy on
next boot).

```bash
# Via uf2_upload.py (recommended — handles bootloader entry + UF2 copy)
python3 tools/uf2_upload.py /dev/ttyACM0 \
    --uf2 bootloader/update-bootloader-qspi-ota-default.uf2

# Via make target (update Makefile's BOOTLOADER_UF2 to point at this file
# before running, currently still references update-bootloader-ota-default.uf2)
make bootloader-update UPLOAD_PORT=/dev/ttyACM0
```

**Always test on a bench device first.** Verify the new bootloader boots
the existing app, then erase the app region (SWD) to confirm DFU mode
defaults to BLE OTA (not USB UF2). Only after both pass: flash sculpture
units.

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
