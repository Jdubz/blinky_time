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
| **`update-bootloader-qspi-ota-default.uf2`** | **May 13** | **RAM magic + QSPI + DEFAULT_TO_OTA_DFU + dual-transport DFU mode + SD-aware flash** | **Current target for sculpture installs (F1 in `docs/SCULPTURE_BLE_RECOVERY_PLAN.md`)** |
| `update-bootloader-qspi-ota-default_s140_7.3.0.zip` | May 13 | Same + SoftDevice | DFU.zip form, for OTA bootloader update via fleet server |

### Fourth critical fix in current build (`0.8.0-5-gd7be9be`): SD-aware flash writes

`0.8.0-4-g76d1e60` introduced dual-transport DFU but landed broken — the USB MSC
UF2 path silently failed to program internal flash. App UF2s wrote successfully
into the bootloader's virtual FAT (host saw "[PASS] Wrote N/N bytes"), but the
chip's app region stayed erased (verified by reading the bootloader's
`CURRENT.UF2` export back).

Root cause: `flash_nrf5x_write()` goes straight at NVMC via `nrfx_nvmc_*`.
When the SoftDevice is enabled (which dual-transport mode now always does — for
BLE advertising), Nordic's spec says **SD owns NVMC and direct register access
silently no-ops**. The original Adafruit bootloader's USB UF2 mode never
enabled the SoftDevice, so the conflict didn't exist. Dual-transport changed
that without updating the flash path.

Fix: in `flash_nrf5x_flush()`, detect SD state and dispatch.
- SD off → existing `nrfx_nvmc_page_erase` + `nrfx_nvmc_words_write` (unchanged).
- SD on → `sd_flash_page_erase` / `sd_flash_write`, blocking on the
  corresponding `NRF_EVT_FLASH_OPERATION_*` events. The wait loop drains BLE
  and SOC events inline so pstorage / BLE-DFU keep getting their events while
  we hold the main thread (otherwise the completion event would never be
  delivered).

A new hook `flash_nrf5x_sd_event()` is called from `main.c proc_soc()` alongside
the existing `pstorage_sys_event_handler` dispatch. Both handlers see every SoC
event and each tracks its own pending op.

Verified on bench (2026-05-13):
- Test chips D63B / DC9B: bootloader updated via BLE DFU (since their previous
  bootloader was the broken `0.8.0-4` — USB UF2 path couldn't bootstrap itself
  out of the bug). Firmware then deployed via USB MSC UF2 drop, reached app
  mode in ~2 s. ✓

### Recovery from devices stuck on `0.8.0-4-g76d1e60`

If a device is on the broken `0.8.0-4` build, USB UF2 cannot reflash it (same
SD-ownership bug stops both bootloader-update and firmware writes). Use BLE
DFU instead — the BLE DFU code path goes through `pstorage_store` which is
already SD-aware:

```bash
# 1. Build a bootloader-only DFU.zip with the right device-revision.
#    --dev-revision 52840 is REQUIRED for nRF52840 BL-type DFUs — the
#    bootloader's dfu_init_prevalidate() rejects (NRF_ERROR_FORBIDDEN ->
#    result=0x06 OPERATION_FAILED) if device_rev != ADAFRUIT_DEV_REV (52840).
adafruit-nrfutil dfu genpkg \
    --bootloader <bootloader>_nosd.hex \
    --dev-type 0x0052 \
    --dev-revision 52840 \
    --sd-req 0x0123 \
    bootloader-only.zip

# 2. Push over BLE. adafruit-nrfutil bin output pads from address 0 with
#    leading 0xFF bytes (~1 MB) which exceeds the bootloader region size.
#    The push tool must strip leading 0xFFs AND patch the init packet's
#    CRC16 to match the trimmed firmware (see tools/push_ble_dfu.py).
python3 tools/push_ble_dfu.py bootloader-only.zip <MAC>
```

### Critical fix in prior build (`0.8.0-2-g6827504`)

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

## Build Provenance (binary artifacts committed to this repo)

Binary blobs in git can't self-describe their build environment, so the
current set of committed artifacts maps to the following toolchain and
upstream-fork state. Re-derive this section any time a new bootloader UF2
is committed. PR #139 review specifically flagged binary-in-git provenance
as an audit concern; this section is the answer.

**Current artifacts** (`update-bootloader-qspi-ota-default*`,
`update-bootloader-bl-only_rev52840.zip`):
- **Upstream base**: Adafruit_nRF52_Bootloader v0.10.0 (release tag) +
  local fork `blinky-local-patches` branch at commit `d7be9be`
  ("flash_nrf5x: SD-aware page erase + write for dual-transport DFU").
- **Reported version string**: `0.8.0-5-gd7be9be` (the `0.8.0` prefix is
  the upstream version; the `-5-gd7be9be` suffix is `git describe`
  output on the local fork).
- **Compiler**: arm-none-eabi-gcc 9.2.1 (the version bundled with the
  Seeeduino nRF52 1.1.12 Arduino core).
- **Build flags**: `BOARD=xiao_nrf52840_ble_sense DEFAULT_TO_OTA_DFU=1`.
- **SoftDevice bundled in `*_s140_7.3.0.zip`**: Nordic S140 v7.3.0
  (sd_id=0x0123, sd_ver=7003000). The `_nosd.uf2` variant excludes the
  SoftDevice (used for bootloader-only updates over the existing SD).
- **`bootloader-bl-only_rev52840.zip` is a legacy adafruit-nrfutil
  package** with `--dev-type 0x0052 --dev-revision 52840 --sd-req 0x0123`.
  The `52840` device-revision matches what the bootloader's
  `dfu_init_prevalidate` requires (`ADAFRUIT_DEV_REV` for nRF52840 boards);
  using `0xFFFF` was the cause of an INIT_DFU rejection investigated on
  2026-05-13.

The local fork is published at `Jdubz/Adafruit_nRF52_Bootloader`, branch
`blinky-local-patches`. A future PR should add a build matrix in CI that
rebuilds the artifacts from that commit and diffs against the committed
binaries to prove no manual tampering.

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
