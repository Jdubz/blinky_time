# Wireless Communication Plan

*Created: December 2025, Updated: March 28, 2026*

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
  ├── POST /devices/{id}/ota      — firmware upload (UF2 or BLE DFU)
  ├── POST /fleet/ota             — flash all nRF52840 devices
  ├── POST /ota/compile           — compile firmware on server
  └── --no-serial flag            — wireless-only mode
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
| **Server OTA (UF2)** | nRF52840 | `POST /api/devices/{id}/ota` delegates to `tools/uf2_upload.py` |
| **Fleet OTA** | Pi | `POST /api/fleet/ota` flashes all connected nRF52840 sequentially |
| **Compile endpoint** | Pi | `POST /api/ota/compile` runs arduino-cli |
| **UF2 bootloader entry** | nRF52840 | `sd_softdevice_disable()` → DSB/ISB → GPREGRET → DSB/ISB → NVIC_SystemReset |
| **BLE DFU protocol** | nRF52840 | Legacy DFU (SDK v11) fully reverse-engineered: write-without-response, bootloader addr+1, StartNotify, BlueZ cache clearing |
| **Serial port stability** | Pi | DTR toggle on connect, port kept open during bootloader entry |
| **Print& abstraction** | Both | All printDiagnostics() accept Print& — output goes to any transport |
| **Fleet server** | Pi | Serial + BLE + WiFi discovery, dedup, auto-reconnect, REST API |

### Remaining Work

| Feature | Priority | Notes |
|---------|----------|-------|
| **BLE DFU end-to-end transfer** | Medium | START_DFU notification verified. Full firmware transfer never completed. |
| **BLE DFU as OTA fallback** | Medium | Server should try BLE DFU when UF2 fails |
| **WiFi on ESP32-S3** | Low (ESP32 deprioritized) | Hardware antenna issue. BLE is primary wireless transport. |
| **Web Bluetooth (blinky-console)** | Low | Fleet management is via Pi server |

### Known Limitations

- **GPREGRET race condition**: UF2 bootloader entry is ~50-80% per attempt due to MBR interaction. Mitigated by uf2_upload.py's 5-retry logic (>97% cumulative).
- **BlueZ scan filtering**: Pi's BlueZ doesn't always find nRF52840 in BLE scans. Direct connection by address works after initial discovery.
- **BLE discovery stops after connection**: Connected devices stop advertising. Server can't discover new devices while holding connections.

---

## Key Architecture Decisions

### Firmware Output: Print& Abstraction

All diagnostic output (`printDiagnostics`, `showDeviceConfig`, `json info`, etc.) goes through `Print& out` parameter, routed by TeeStream to both USB Serial AND BLE NUS. Zero `Serial.print` calls in command handlers. This enables full wireless operation.

### Server OTA: Delegate to uf2_upload.py

The server's OTA module delegates to `tools/uf2_upload.py` as a subprocess rather than reimplementing upload logic. The tool has 2360 lines of battle-tested safety checks (retries, USB recovery, port validation, firmware verification).

### Bootloader Entry: sd_softdevice_disable + NVIC_SystemReset

All bootloader entry paths (serial command, 1200-baud touch, SafeBootWatchdog) use the same sequence: disable SoftDevice → DSB/ISB barrier → write GPREGRET → DSB/ISB barrier → NVIC_SystemReset. Direct jump approaches were tested but abandoned (broke USB mass storage).

### BLE DFU Protocol (nRF52840)

Adafruit bootloader v0.6.1 uses Legacy DFU (SDK v11). Key findings:
- DFU Control writes must use write-without-response
- Bootloader BLE address = app address + 1 (last octet)
- Must force StartNotify (not AcquireNotify) in bleak
- BlueZ GATT cache must be cleared between app/bootloader connections
- GPREGRET=0xA8 for serial-triggered BLE DFU entry

### ESP32-S3 WiFi: All operations on Core 1

WiFi driver + lwIP not thread-safe across cores. WifiCommandServer uses non-blocking `poll()` on Core 1. BLE is primary wireless transport (WiFi blocked by antenna hardware).

### ESP32-S3 BLE: NimBLE-Arduino required

Built-in NimBLE in arduino-esp32 3.3.7 crashes on ESP32-S3. External NimBLE-Arduino v2.4.0 fixes this.

---

## BLE Addresses (blinkyhost devices)

| Serial Number | App BLE Address | Bootloader BLE Address | USB Hub |
|--------------|-----------------|----------------------|---------|
| 06ACEB | F4:15:6D:FA:4D:93 | F4:15:6D:FA:4D:94 | 1-1.2 port 3 |
| 2A798E | unknown | unknown+1 | 1-1.2 port 2 |
| 659C8D (Long Tube) | FA:E6:7D:A9:8B:3A | FA:E6:7D:A9:8B:3B | 1-1.2 port 4 |

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
