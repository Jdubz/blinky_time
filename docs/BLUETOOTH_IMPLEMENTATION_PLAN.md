# Wireless Communication Plan

*Created: December 2025, Updated: March 30, 2026. Status note added October 21, 2026.*

> **Status (2026-10-21):** BLE fleet management over the Pi server is done and in daily use; most of this doc is the record of how it was built. ESP32-S3 (and its WiFi transport) was cut in March 2026 and lives on only in the next-gen plan (`docs/NEXTGEN_ESP32S3_PLAN.md`). What changed since March:
> - **Flashing** goes through one entry point, `FleetManager.flash_device()` (`docs/FLASH_LOCKDOWN_PLAN.md`), and operators use only `./scripts/deploy.sh`, which installs **stable releases only** (`docs/RELEASE_PROCESS.md`). Device-mutating routes need the `X-Deploy-Tool` header that only `deploy.sh` sets.
> - **Fleet bootloader is 0.8.0-10**; its BLE DFU accepts up to 244-byte packets (ATT MTU 247), but transfers still negotiate MTU 23 in practice (#216).
> - **Web Bluetooth in blinky-console** exists now: phone record mode (`docs/PHONE_RECORD_MODE_PLAN.md`) and a signed-OTA update path that is built but hidden (`docs/WEB_FLASHER.md`).
> - **Open BLE work** is tracked in `docs/BLE_FLEET_RELIABILITY_PLAN.md` and GitHub issues (#215, #216, #221, #190, #173).

## Goal

Enable wireless device management so the Pi fleet server (blinky-server) can manage nRF52840 and ESP32-S3 devices without USB serial.

## Architecture Overview

```
Pi fleet server (blinky-server)
  ├── USB Serial (SerialTransport — working)
  ├── BLE NUS (BleTransport — working, both platforms)
  └── WiFi TCP (WifiTransport — implemented, blocked by ESP32-S3 antenna)

REST API: http://blinkyhost.local:8420/api/
  ├── GET  /devices               — list all devices (incl. RSSI, MTU, BLE addr)
  ├── GET  /fleet/status          — fleet health summary (counts, per-device health)
  ├── POST /fleet/discover        — trigger immediate BLE/serial scan
  ├── POST /devices/{id}/command  — send command
  ├── POST /devices/{id}/flash    — firmware upload (UF2 or BLE DFU, auto-detected)
  ├── POST /fleet/upload          — stage firmware for a fleet flash (deploy.sh only)
  ├── POST /fleet/flash           — flash nRF52840 devices (deploy.sh only)
  ├── POST /flash-jobs            — create one flash job (deploy.sh only)
  ├── POST /fleet/settings/save   — save settings on all devices
  ├── POST /fleet/settings/load   — load settings on all devices
  ├── POST /fleet/settings/defaults — restore defaults on all devices
  └── --no-serial flag            — BLE-only fleet management mode
```

The `/firmware/compile*` routes are gone. `POST /fleet/deploy` (server-side compile + flash) still exists in `routes_firmware.py` but contradicts the stable-only rule; don't use it. Flash fleet devices only with `./scripts/deploy.sh`.

**Use case**: Fleet management — settings changes, generator/effect selection (the scene system was retired 2026-05-19; see `archive/OPEN_ISSUES_2026_05_19.md` §1.2), device configuration, status monitoring, OTA firmware updates.

## Current Status (March 31, 2026)

### Working

| Feature | Platform | Notes |
|---------|----------|-------|
| **BLE NUS bidirectional** | nRF52840 | All command output routed through TeeStream → BLE NUS via Print& refactor |
| **BLE NUS bidirectional** | ESP32-S3 | NUS TX wired via setEsp32BleNus(), `show nn` verified over BLE |
| **BLE device discovery** | Pi | 6 devices found wirelessly (3 nRF52840 + 2 ESP32-S3 + 1 extra nRF52840) |
| **BLE connections stable** | nRF52840 | vTaskDelay(1) replaces no-op yield(), StartNotify for reliable notifications |
| **Wireless-only mode** | Pi | `--no-serial` flag, all 6 devices managed via BLE only |
| **Platform detection** | Both | Firmware reports `"platform":"nrf52840"` or `"esp32s3"` in `json info` |
| **Cross-transport identity** | Both | Firmware reports `"sn"` (FICR DEVICEID) and `"ble"` (BLE MAC) in `json info`. Server uses `hardware_sn` for dedup. |
| **BLE disconnect detection** | Pi | `BleTransport` registers bleak `disconnected_callback` → auto-transitions Device to DISCONNECTED → fleet auto-reconnects |
| **BLE liveness checks** | Pi | Background loop pings BLE devices every ~30s if no recent comms, detects silent disconnects |
| **DFU recovery detection** | Pi | Discovery scans for DFU service UUID (00001530), detects devices in SafeBoot BLE DFU bootloader, surfaces `dfu_recovery` state |
| **No-fallback dispatch** | Pi | UF2 for serial devices, BLE DFU for BLE-only devices. No silent fallback — failures are returned immediately for investigation |
| **Server flash (UF2)** | nRF52840 | `POST /api/devices/{id}/flash` delegates to `uf2_upload.py` for serial devices |
| **BLE DFU flash** | nRF52840 | `POST /api/devices/{id}/flash` auto-detects BLE transport, does full DFU transfer wirelessly (~5.5 min/device) |
| **Fleet flash** | Pi | `POST /api/fleet/flash` flashes all connected nRF52840 sequentially (serial + BLE mixed) |
| **Fleet deploy** | Pi | Superseded: `./scripts/deploy.sh` installs the latest stable release (`POST /api/fleet/deploy` compiled locally; don't use it) |
| **BLE DFU proven** | nRF52840 | End-to-end tested Mar 30: 510KB in ~5.5 min, 2/2 fleet flash success, auto-reconnect after DFU |
| **DFU zip generation** | Pi | Pure-Python (`firmware/compile.py`), no adafruit-nrfutil dependency. The compile endpoints were removed |
| **UF2 bootloader entry** | nRF52840 | Write RAM magic (0xBEEF0057) to 0x20007F7C → DSB/ISB → NVIC_SystemReset (custom bootloader checks RAM before GPREGRET) |
| **BLE DFU protocol** | nRF52840 | Legacy DFU (SDK v11): write-with-response on control, 60s START_DFU timeout for flash erase, chunk size from the negotiated MTU (20 bytes at MTU 23, up to 244), word-aligned padding |
| **BlueZ stale cleanup** | Pi | Auto-disconnects stale BLE connections from previous server sessions on startup |
| **BLE reconnect backoff** | Pi | Exponential backoff for failing BLE devices (10s, 20s, 40s... capped at 5 min) |
| **Discovery pause** | Pi | Background BLE discovery pauses during DFU to avoid BleakScanner conflicts |
| **BLE connect timeout** | Pi | Hard 25s timeout wrapping BLE connect sequence (includes GATT rediscovery after cache clear) |
| **Serial port stability** | Pi | DTR toggle on connect, port kept open during bootloader entry |
| **Print& abstraction** | Both | All printDiagnostics() accept Print& — output goes to any transport |
| **Fleet server** | Pi | Serial + BLE + WiFi discovery, dedup, auto-reconnect, REST API |
| **API enrichment** | Pi | `GET /devices` includes `hardware_sn`, `ble_address`, `rssi`, `mtu`, `last_seen` per device |
| **Fleet health API** | Pi | `GET /fleet/status` returns aggregate fleet stats (counts by state/transport, per-device health) |
| **Manual discovery** | Pi | `POST /fleet/discover` triggers immediate BLE/serial scan, returns new devices |
| **Fleet settings ops** | Pi | `POST /fleet/settings/save\|load\|defaults` — fleet-wide settings management |
| **BLE RSSI updates** | Pi | RSSI refreshed from BlueZ D-Bus during liveness pings (~30s), not just discovery-time |
| **Serial liveness checks** | Pi | Serial devices now get periodic `json info` pings (~30s) to detect stale connections |
| **BLE GATT cache clear** | Pi | `bluetoothctl remove` before each BLE connect prevents notification stacking across reconnections |
| **Failure limit** | Pi | BLE devices that fail 10 consecutive connection attempts are removed from the fleet |

### Remaining Work

| Feature | Priority | Notes |
|---------|----------|-------|
| **Post-DFU USB re-enumeration** | Medium | After BLE DFU, USB serial doesn't re-enumerate (host-side issue). BLE reconnects fine. Not blocking for BLE-only fleet management. |
| **BLE-DFU MTU** | Medium | Transfers negotiate MTU 23 (20-byte chunks, ~6.5 min per device) although the bootloader allows 247 (#216) |
| **WiFi on ESP32-S3** | Dropped | ESP32-S3 cut March 2026; next gen only |
| **Web Bluetooth (blinky-console)** | Done in part | Record mode works; Bluetooth firmware updates are built but hidden until they pass on hardware (`docs/WEB_FLASHER.md`) |

### Known Limitations

- **GPREGRET race condition (RESOLVED)**: Replaced GPREGRET with RAM magic at 0x20007F7C via custom bootloader (Apr 2026). Bootloader entry now succeeds on first attempt. See `docs/archive/UPLOAD_OVERHAUL_PLAN.md` for the original analysis; live invariants enforced by `scripts/verify_bootloader.py`.
- **BLE DFU transfer speed**: ~1.7 KB/s (20-byte BLE packets), ~5.5–6.5 min per device. Sequential only (Pi's BLE adapter handles one DFU at a time). Larger packets are possible but not negotiated yet (#216).
- **Post-DFU USB**: After BLE DFU boot, USB serial doesn't re-enumerate without physical power cycle. uhubctl on Pi doesn't fully cut power. BLE reconnection works fine.
- **BlueZ stale connections**: Server restart leaves stale BLE connections in BlueZ. Auto-cleanup runs on startup. Per-device `bluetoothctl disconnect` runs before each BLE connect to prevent notification stacking.
- **BLE discovery stops after connection**: Connected devices stop advertising. Server can't discover new devices while holding connections.
- **BLE throughput at weak signal**: Devices with RSSI < -80 dBm may have BLE throughput as low as 1 notification/sec. Large responses (e.g., `json settings`) may time out. Commands and `json info` work reliably.

### Bugs Fixed (Mar 31, 2026)

- **Dedup false positives**: Previously deduped by `device_type:device_name` (config-based), which matched different physical devices with the same config. Now only dedupes by `hardware_sn` (chip-unique FICR DEVICEID).
- **Dedup thrashing loop**: Deduped BLE devices were immediately rediscovered and reconnected on the next cycle. Now tracked in an exclusion set, cleared when serial device disconnects.
- **BLE notification stacking**: Repeated connect/disconnect cycles caused BlueZ to stack GATT subscriptions, delivering each notification N times (garbled responses). Fixed by running `bluetoothctl disconnect` before each BLE connect.
- **Release hold ID mismatch**: `release_device()` stored reconnect blackout under the shortened API ID (e.g., `ABFBC412`) instead of the full device ID (`ABFBC41283E2D211`), causing the hold to be silently ignored.

---

## Key Architecture Decisions

### Firmware Output: Print& Abstraction

All diagnostic output (`printDiagnostics`, `showDeviceConfig`, `json info`, etc.) goes through `Print& out` parameter, routed by TeeStream to both USB Serial AND BLE NUS. Zero `Serial.print` calls in command handlers. This enables full wireless operation.

### Server OTA: Delegate to uf2_upload.py

The server's OTA module delegates to `tools/uf2_upload.py` as a subprocess rather than reimplementing upload logic. The tool has 2360 lines of battle-tested safety checks (retries, USB recovery, port validation, firmware verification).

### Bootloader Entry: RAM Magic + NVIC_SystemReset

All bootloader entry paths (serial command, 1200-baud touch, SafeBootWatchdog, QSPI OTA commit) write a magic value to RAM at `0x20007F7C` → DSB/ISB → NVIC_SystemReset. Custom bootloader (Adafruit fork) checks this address before GPREGRET. RAM survives system reset (proven by double-reset detection at the same address). No `Serial.flush()` before reset — commands may arrive over BLE NUS.

### BLE DFU Protocol (nRF52840) — Proven End-to-End (Mar 30, 2026)

The bootloader (stock Adafruit v0.6.1 then; the fleet's 0.8.0-10 fork now) uses Legacy DFU (SDK v11). DFU Revision = 0x0008 (v0.8, NOT Secure DFU).

**Protocol sequence**: START_DFU (with 60s timeout for flash erase) → INIT_DFU (12-byte init packet with CRC-16) → SET_PRN (interval=8) → RECEIVE_FW (20-byte chunks, word-aligned) → VALIDATE → ACTIVATE_AND_RESET.

**Key protocol details** (all verified via HCI capture and testing):
- DFU Control writes MUST use write-with-response (`response=True` in bleak)
- DFU Packet writes use write-without-response (`response=False`)
- START_DFU triggers flash erase: ~25s for 500KB firmware — needs 60s timeout
- Bootloader capped payload at 20 bytes regardless of negotiated MTU (0.8.0-10 accepts up to 244; see #216)
- Last firmware chunk must be padded to 4-byte word alignment
- Bootloader BLE address = app address + 1 (last octet)
- Must force StartNotify (not AcquireNotify) in bleak for reliable notifications
- BlueZ GATT cache must be cleared between app/bootloader connections
- RAM magic 0xBEEF00A8 for BLE DFU entry (via serial `bootloader ble` or BLE NUS command)
- Bootloader retains DFU state across BLE disconnections (need power cycle or SYS_RESET 0x06 to clear)
- DFU zip generated via pure-Python (`compile.py`) — adafruit-nrfutil broken on Python 3.13
- Init packet: device_type=0x0052, sd_req=0xFFFE, CRC-16/CCITT of firmware binary

**Test results**: 510 KB firmware transferred in ~5.5 min per device. Fleet OTA: 2/2 devices flashed successfully with auto-reconnect.

### ESP32-S3 WiFi: All operations on Core 1

WiFi driver + lwIP not thread-safe across cores. WifiCommandServer uses non-blocking `poll()` on Core 1. BLE is primary wireless transport (WiFi blocked by antenna hardware).

### ESP32-S3 BLE: NimBLE-Arduino required

Built-in NimBLE in arduino-esp32 3.3.7 crashes on ESP32-S3. External NimBLE-Arduino v2.4.0 fixes this.

---

## BLE Addresses (blinkyhost devices)

| Serial Number | App BLE Address | Bootloader BLE Address | USB Hub | Notes |
|--------------|-----------------|----------------------|---------|-------|
| ABFBC412 | E3:8D:10:5F:17:66 | E3:8D:10:5F:17:67 | 1-1.2 port 3 | Bare chip, BLE DFU tested |
| 2A798EF8 | CF:52:2F:EF:C5:23 | CF:52:2F:EF:C5:24 | 1-1.2 port 1 | Bare chip, BLE DFU tested |
| 659C8DD3 (Long Tube) | FA:E6:7D:A9:8B:3A | FA:E6:7D:A9:8B:3B | 1-1.2 port 2 | Installed, BLE connect unreliable |
| 062CBD12 | unknown | unknown+1 | 1-1.2 port 4 | DO NOT TOUCH (SWD recovered) |
| (remote) | E9:A8:5C:A5:BB:BE | E9:A8:5C:A5:BB:BF | BLE only | Bare chip, BLE DFU tested |

---

## Resource Usage

These numbers are from March 2026. A b215 image is about 485 KB.

### nRF52840 (with NUS + DFU + Print& refactor)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~394 KB (48%) | 811 KB | 417 KB (51%) |
| RAM | 24.4 KB (10%) | 237 KB | 213 KB (90%) |

### ESP32-S3 (with BLE + WiFi + OTA; retired platform)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~1.45 MB (43%) | 3.3 MB | 1.9 MB (57%) |
| RAM | 60.5 KB (18%) | 327 KB | 267 KB (82%) |
