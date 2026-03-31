# Wireless Communication Plan

*Created: December 2025, Updated: March 30, 2026*

## Goal

Enable wireless device management so the Pi fleet server (blinky-server) can manage nRF52840 and ESP32-S3 devices without USB serial.

## Architecture Overview

```
Pi fleet server (blinky-server)
  ├── USB Serial (SerialTransport — working)
  ├── BLE NUS (BleTransport — working, both platforms)
  └── WiFi TCP (WifiTransport — implemented, blocked by ESP32-S3 antenna)

REST API: http://blinkyhost.local:8420/api/
  ├── GET  /devices               — list all devices
  ├── POST /devices/{id}/command  — send command
  ├── POST /devices/{id}/ota      — firmware upload (UF2 or BLE DFU, auto-detected)
  ├── POST /fleet/ota             — flash all nRF52840 devices (accepts .hex or .dfu.zip)
  ├── POST /fleet/deploy          — compile + generate DFU zip + flash all devices
  ├── POST /ota/compile           — compile firmware
  ├── POST /ota/compile-dfu       — compile + generate DFU zip
  └── --no-serial flag            — BLE-only fleet management mode
```

**Use case**: Fleet management — settings changes, mode/scene selection, device configuration, status monitoring, OTA firmware updates.

## Current Status (March 28, 2026)

### Working

| Feature | Platform | Notes |
|---------|----------|-------|
| **BLE NUS bidirectional** | nRF52840 | All command output routed through TeeStream → BLE NUS via Print& refactor |
| **BLE NUS bidirectional** | ESP32-S3 | NUS TX wired via setEsp32BleNus(), `show nn` verified over BLE |
| **BLE device discovery** | Pi | 6 devices found wirelessly (3 nRF52840 + 2 ESP32-S3 + 1 extra nRF52840) |
| **BLE connections stable** | nRF52840 | vTaskDelay(1) replaces no-op yield(), StartNotify for reliable notifications |
| **Wireless-only mode** | Pi | `--no-serial` flag, all 6 devices managed via BLE only |
| **Platform detection** | Both | Firmware reports `"platform":"nrf52840"` or `"esp32s3"` in `json info` |
| **Server OTA (UF2)** | nRF52840 | `POST /api/devices/{id}/ota` delegates to `tools/uf2_upload.py` for serial devices |
| **BLE DFU OTA** | nRF52840 | `POST /api/devices/{id}/ota` auto-detects BLE transport, does full DFU transfer wirelessly (~5.5 min/device) |
| **Fleet OTA** | Pi | `POST /api/fleet/ota` flashes all connected nRF52840 sequentially (serial + BLE mixed) |
| **Fleet deploy** | Pi | `POST /api/fleet/deploy` one-shot compile + DFU zip + flash all devices |
| **BLE DFU proven** | nRF52840 | End-to-end tested Mar 30: 510KB in ~5.5 min, 2/2 fleet OTA success, auto-reconnect after DFU |
| **Compile endpoint** | Pi | `POST /api/ota/compile` and `/ota/compile-dfu` (pure-Python DFU zip, no adafruit-nrfutil dependency) |
| **UF2 bootloader entry** | nRF52840 | `sd_softdevice_disable()` → DSB/ISB → GPREGRET → DSB/ISB → NVIC_SystemReset |
| **BLE DFU protocol** | nRF52840 | Legacy DFU (SDK v11): write-with-response on control, 60s START_DFU timeout for flash erase, 20-byte chunks, word-aligned padding |
| **BlueZ stale cleanup** | Pi | Auto-disconnects stale BLE connections from previous server sessions on startup |
| **BLE reconnect backoff** | Pi | Exponential backoff for failing BLE devices (10s, 20s, 40s... capped at 5 min) |
| **Discovery pause** | Pi | Background BLE discovery pauses during DFU to avoid BleakScanner conflicts |
| **BLE connect timeout** | Pi | Hard 20s timeout wrapping BLE connect sequence (prevents server hang on stuck connections) |
| **Serial port stability** | Pi | DTR toggle on connect, port kept open during bootloader entry |
| **Print& abstraction** | Both | All printDiagnostics() accept Print& — output goes to any transport |
| **Fleet server** | Pi | Serial + BLE + WiFi discovery, dedup, auto-reconnect, REST API |

### Remaining Work

| Feature | Priority | Notes |
|---------|----------|-------|
| **Post-DFU USB re-enumeration** | Medium | After BLE DFU, USB serial doesn't re-enumerate (host-side issue). BLE reconnects fine. Not blocking for BLE-only fleet management. |
| **BLE DFU as UF2 fallback** | Medium | Server should try BLE DFU when UF2 fails for installed devices |
| **WiFi on ESP32-S3** | Low (ESP32 deprioritized) | Hardware antenna issue. BLE is primary wireless transport. |
| **Web Bluetooth (blinky-console)** | Low | Fleet management is via Pi server |

### Known Limitations

- **GPREGRET race condition**: UF2 bootloader entry is ~50-80% per attempt due to MBR interaction. Mitigated by uf2_upload.py's 5-retry logic (>97% cumulative).
- **BLE DFU transfer speed**: ~1.7 KB/s (20-byte BLE packets), ~5.5 min per device for 510 KB firmware. Sequential only (Pi's BLE adapter handles one DFU at a time).
- **Post-DFU USB**: After BLE DFU boot, USB serial doesn't re-enumerate without physical power cycle. uhubctl on Pi doesn't fully cut power. BLE reconnection works fine.
- **BlueZ stale connections**: Server restart leaves stale BLE connections in BlueZ. Auto-cleanup runs on startup but some devices (e.g., FA:E6:7D:A9:8B:3A) resist disconnect.
- **BLE discovery stops after connection**: Connected devices stop advertising. Server can't discover new devices while holding connections.

---

## Key Architecture Decisions

### Firmware Output: Print& Abstraction

All diagnostic output (`printDiagnostics`, `showDeviceConfig`, `json info`, etc.) goes through `Print& out` parameter, routed by TeeStream to both USB Serial AND BLE NUS. Zero `Serial.print` calls in command handlers. This enables full wireless operation.

### Server OTA: Delegate to uf2_upload.py

The server's OTA module delegates to `tools/uf2_upload.py` as a subprocess rather than reimplementing upload logic. The tool has 2360 lines of battle-tested safety checks (retries, USB recovery, port validation, firmware verification).

### Bootloader Entry: sd_softdevice_disable + NVIC_SystemReset

All bootloader entry paths (serial command, 1200-baud touch, SafeBootWatchdog) use the same sequence: disable SoftDevice → DSB/ISB barrier → write GPREGRET → DSB/ISB barrier → NVIC_SystemReset. Direct jump approaches were tested but abandoned (broke USB mass storage).

### BLE DFU Protocol (nRF52840) — Proven End-to-End (Mar 30, 2026)

Adafruit bootloader v0.6.1 uses Legacy DFU (SDK v11). DFU Revision = 0x0008 (v0.8, NOT Secure DFU).

**Protocol sequence**: START_DFU (with 60s timeout for flash erase) → INIT_DFU (12-byte init packet with CRC-16) → SET_PRN (interval=8) → RECEIVE_FW (20-byte chunks, word-aligned) → VALIDATE → ACTIVATE_AND_RESET.

**Key protocol details** (all verified via HCI capture and testing):
- DFU Control writes MUST use write-with-response (`response=True` in bleak)
- DFU Packet writes use write-without-response (`response=False`)
- START_DFU triggers flash erase: ~25s for 500KB firmware — needs 60s timeout
- Bootloader caps payload at 20 bytes regardless of negotiated MTU
- Last firmware chunk must be padded to 4-byte word alignment
- Bootloader BLE address = app address + 1 (last octet)
- Must force StartNotify (not AcquireNotify) in bleak for reliable notifications
- BlueZ GATT cache must be cleared between app/bootloader connections
- GPREGRET=0xA8 for BLE DFU entry (via serial `bootloader ble` or BLE NUS command)
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

### nRF52840 (with NUS + DFU + Print& refactor)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~394 KB (48%) | 811 KB | 417 KB (51%) |
| RAM | 24.4 KB (10%) | 237 KB | 213 KB (90%) |

### ESP32-S3 (with BLE + WiFi + OTA)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~1.45 MB (43%) | 3.3 MB | 1.9 MB (57%) |
| RAM | 60.5 KB (18%) | 327 KB | 267 KB (82%) |
