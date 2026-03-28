# Wireless Communication Plan

*Created: December 2025, Updated: March 27, 2026*

## Goal

Enable wireless device management so the Pi fleet server (blinky-server) can manage nRF52840 and ESP32-S3 devices without USB serial.

## Architecture Overview

```
Pi fleet server (blinky-server)
  ├── USB Serial (SerialTransport — working)
  ├── BLE NUS (BleTransport — working, both platforms)
  └── WiFi TCP (WifiTransport — implemented, blocked by ESP32-S3 antenna)

Web browser (blinky-console)
  ├── WebSerial (working)
  └── Web Bluetooth NUS (future)
```

**Use case**: Fleet management — settings changes, mode/scene selection, device configuration, status monitoring, OTA firmware updates. No real-time streaming over wireless.

## Current Status (March 27, 2026)

### Completed

| Feature | Platform | Status | Notes |
|---------|----------|--------|-------|
| **BLE NUS** | nRF52840 | Working | Bluefruit52Lib, SoftDevice S140, verified via bleak |
| **BLE NUS** | ESP32-S3 | Compiled, untested | NimBLE-Arduino 2.4.0, Esp32BleNus.h/cpp |
| **BLE Advertiser** | ESP32-S3 | Working | Ported to NimBLE 2.x API |
| **BLE Scanner** | nRF52840 | Working | Passive scan for advertising packets |
| **BLE DFU service** | nRF52840 | Working | BLEDfu (Bluefruit52Lib), app→bootloader transition verified |
| **WiFi TCP server** | ESP32-S3 | Working | Non-blocking poll() on Core 1 |
| **WiFi OTA (ArduinoOTA)** | ESP32-S3 | Code complete | Needs stable WiFi signal to test (antenna issue) |
| **WiFi HTTP OTA** | ESP32-S3 | Code complete | `wifi ota <url>` serial command |
| **mDNS advertisement** | ESP32-S3 | Code complete | `_blinky._tcp` on port 3333 |
| **Fleet server (serial)** | Pi | Working | 4 serial devices auto-discovered and managed |
| **Fleet server (BLE)** | Pi | Working | 2 BLE devices auto-discovered via bleak |
| **Multi-transport discovery** | Pi | Working | Serial (VID/PID) + BLE (NUS UUID) + WiFi (mDNS + static) |
| **Device deduplication** | Pi | Working | Prefers serial over BLE for same physical device |
| **BLE protocol timeouts** | Pi | Done | 500ms line gap for BLE (vs 100ms serial) |
| **Fleet server launcher** | Pi | Working | `run.sh` (tmux + auto-restart), crontab @reboot |
| **REST API** | Pi | Working | FastAPI on port 8420 |

### Blocked

| Feature | Blocker | Notes |
|---------|---------|-------|
| **WiFi on ESP32-S3** | Hardware antenna | XIAO ESP32-S3 Sense routes to u.FL connector only. No external antenna = -70 dBm at close range. WiFi code is correct but connection too unstable. |
| **BLE DFU transfer** | Protocol rewritten (testing) | App→bootloader works. Bootloader uses **Legacy DFU (SDK v11)** — DFU Revision 0x0008 is the bootloader version, NOT Secure DFU v2. Previous code sent wrong opcodes causing GATT 0x0E. `ble_dfu.py` rewritten to Legacy DFU protocol (Mar 28). |
| **BLE scan from Pi** | BlueZ filtering | Direct connection by address works (e.g. F4:15:6D:FA:4D:93 for 06ACEB). `bleak` scan doesn't find nRF52840 devices — BlueZ scan filtering issue, not radio. |

### Not Started

| Feature | Priority | Notes |
|---------|----------|-------|
| Web Bluetooth (blinky-console) | Low | Fleet management is via Pi server |
| Device registry persistence | Low | In-memory discovery works |

---

## Key Architecture Decisions

### ESP32-S3 WiFi: All operations on Core 1

ESP32-S3 WiFi driver + lwIP are NOT thread-safe across cores. `WiFi.status()`, `server.available()`, and client read/write must all run on the same core. The WifiCommandServer uses non-blocking `poll()` in the main loop (Core 1). WiFi.begin() blocks once in setup (up to 10s). HWCDC Serial output doesn't work from Core 0.

### ESP32-S3 BLE: NimBLE-Arduino required

Built-in NimBLE in arduino-esp32 3.3.7 crashes on ESP32-S3 (`npl_freertos_mutex_init` assertion failure, issues #12357/#12362). External NimBLE-Arduino v2.3.8+ fixes this. Install: `arduino-cli lib install "NimBLE-Arduino"`. BleAdvertiser uses `NimBLEDevice` API (NimBLE 2.x dropped old BLEDevice aliases).

### nRF52840 BLE DFU: Secure DFU v2

Adafruit nRF52 bootloader v0.6.2 uses **Legacy DFU (SDK v11)** protocol. The DFU Revision characteristic reports 0x0008, but this is the bootloader version number (DFU_REV_MAJOR=0x00, DFU_REV_MINOR=0x08), NOT a Secure DFU v2 protocol indicator. The app-side BLEDfu service (Bluefruit52Lib) triggers bootloader entry via GPREGRET=0xB1 (write 0x01 to DFU Control in app mode). The bootloader re-advertises as "AdaDFU" with DFU service UUID. Reconnection requires clearing BlueZ GATT cache between connections. BLE scanning from the Pi doesn't find nRF52840 devices (BlueZ scan filtering issue), but direct connection by address works (e.g. device 06ACEB at F4:15:6D:FA:4D:93). Previous blocker (GATT 0x0E Unlikely Error) was caused by sending Secure DFU v2 opcodes to a Legacy DFU bootloader — `ble_dfu.py` rewritten to use correct Legacy DFU protocol (Mar 28).

### ESP32-S3 WiFi antenna

XIAO ESP32-S3 Sense routes RF to u.FL connector only (hardware resistor, no software switch). Without external antenna module attached, RSSI is -70 dBm at close range. WiFi functional but unreliable. BLE is the primary wireless transport.

---

## Firmware Components

### nRF52840

| File | Purpose |
|------|---------|
| `comms/BleNus.h/cpp` | NUS peripheral (serial-over-BLE) |
| `comms/BleScanner.h/cpp` | Passive BLE scan for fleet broadcasts |
| `services/BLEDfu` (Bluefruit52Lib) | DFU service for wireless firmware updates |

### ESP32-S3

| File | Purpose |
|------|---------|
| `comms/BleAdvertiser.h/cpp` | BLE broadcast (NimBLE 2.x API) |
| `comms/Esp32BleNus.h/cpp` | NUS peripheral (NimBLE 2.x API) |
| `comms/WifiManager.h/cpp` | WiFi credential storage (NVS) |
| `comms/WifiCommandServer.h/cpp` | TCP server (non-blocking Core 1) |

### blinky-server (Pi)

| File | Purpose |
|------|---------|
| `transport/serial_transport.py` | USB serial (pyserial-asyncio) |
| `transport/ble_transport.py` | BLE NUS (bleak) |
| `transport/wifi_transport.py` | WiFi TCP (asyncio streams) |
| `transport/discovery.py` | Multi-transport device discovery |
| `device/manager.py` | Fleet manager with dedup + auto-reconnect |
| `device/protocol.py` | Command/response with transport-aware timeouts |
| `run.sh` | tmux launcher with auto-restart |

### Tools

| File | Purpose |
|------|---------|
| `tools/ble_dfu.py` | BLE DFU upload tool (app→bootloader working, transfer WIP) |

---

## Testing Results (March 27, 2026)

### Fleet Server on blinkyhost

6 devices discovered and managed:
- 2x nRF52840 via serial (Test Chip, Long Tube)
- 2x ESP32-S3 via serial (Hat, Hat)
- 2x nRF52840 via BLE (Test Chip RSSI=-36, Tube Light RSSI=-88)

REST API verified:
- `GET /api/devices` — lists all 6 devices
- `POST /api/devices/{id}/command` — command execution over BLE
- `POST /api/fleet/command` — fleet-wide broadcast

### BLE DFU (nRF52840)

- `ble_dfu.py --scan` discovers DFU-capable devices
- App→bootloader transition works: write 0x01 to DFU Control, device reboots into "AdaDFU" mode
- Reconnection to bootloader succeeds (must clear BlueZ GATT cache between connections)
- BLE scanning from Pi doesn't find nRF52840 devices (BlueZ scan filtering issue); direct connection by address works (e.g. F4:15:6D:FA:4D:93 for 06ACEB)
- Bootloader reports DFU Revision 0x0008 (bootloader version, **Legacy DFU protocol**)
- Previous firmware transfer blocker (GATT 0x0E) was caused by sending Secure DFU v2 opcodes to Legacy DFU bootloader
- `ble_dfu.py` rewritten to use Legacy DFU (SDK v11) protocol (Mar 28) — testing pending

---

## Resource Usage

### nRF52840 (with NUS + DFU)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~393 KB (48%) | 811 KB | 418 KB (52%) |
| RAM | 24.4 KB (10%) | 237 KB | 213 KB (90%) |

### ESP32-S3 (with BLE + WiFi + OTA)

| Resource | Usage | Available | Headroom |
|----------|-------|-----------|----------|
| Flash | ~1.45 MB (43%) | 3.3 MB | 1.9 MB (57%) |
| RAM | 60.5 KB (18%) | 327 KB | 267 KB (82%) |
