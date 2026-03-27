# Wireless Communication Plan

*Created: December 2025, Updated: March 2026*

## Goal

Enable wireless device management so the Pi fleet server (blinky-server) can manage nRF52840 and ESP32-S3 devices without USB serial. The blinky-console web app can also connect directly to any device via BLE.

## Architecture Overview

```
Pi fleet server (blinky-server)
  ├── USB Serial (SerialTransport — existing, working)
  ├── BLE NUS (BleTransport — nRF52840 devices, uses bleak)
  └── WiFi TCP (WifiTransport — ESP32-S3 devices, future)

Web browser (blinky-console)
  ├── WebSerial (existing, working)
  └── Web Bluetooth NUS (future)
```

**Use case**: Fleet management — settings changes, mode/scene selection, device configuration, status monitoring. No real-time streaming or time-dependent data over wireless.

## Current Status (March 2026)

### Completed: NUS on nRF52840 (Step 1)

BLE Nordic UART Service is implemented, compiled, flashed, and **verified working** on nRF52840 devices. Bidirectional serial-over-BLE communication is functional.

**What was built:**

1. **`comms/BleNus.h/cpp`** — NUS peripheral with paced output
   - Inherits from `Print` so it plugs directly into TeeStream as secondary output
   - 4 KB TX ring buffer with paced drain (4 MTU-chunks per main loop iteration)
   - RX line assembly with callback to SerialConsole::handleCommand()
   - MTU 247 negotiation on connect
   - Connection/disconnection handling with buffer cleanup
   - Diagnostics: MTU, RSSI, uptime, TX buffer utilization, line counts

2. **`inputs/SerialConsole.h` — TeeStream** class
   - Print adapter that duplicates output to USB Serial + BLE NUS
   - Simple pass-through (no line buffering needed — BleNus handles pacing)
   - Secondary set to BleNus* (which is-a Print)

3. **`inputs/SerialConsole.cpp` — Output routing**
   - All ~445 `Serial.print()` calls replaced with `out_.print()` (routed through TeeStream)
   - Static log methods (logDebug/Info/Warn/Error) use `instance_->out_` with Serial fallback
   - `setBleNus()` wires up TeeStream secondary and SettingsRegistry output
   - `handleCommand()` made public for BLE callback routing
   - `handleBleCommand()` extended with `ble nus` subcommand for NUS diagnostics

4. **`config/SettingsRegistry.h/cpp` — Output routing**
   - Added `setOutput(Print*)` method
   - All ~93 `Serial.print()` calls replaced with `out_->print()`

5. **`blinky-things.ino` — BLE initialization**
   - Bluefruit.begin(1, 0) in main sketch (shared between NUS + Scanner)
   - BleNus initialized before BleScanner
   - NUS RX callback routes to SerialConsole::handleCommand()
   - bleNus.update() called in main loop

6. **`comms/BleScanner.cpp` — Decoupled from Bluefruit init**
   - Removed Bluefruit.begin()/setName()/setTxPower() (now in main sketch)

**Verified working via bleak from Pi:**
- `json info` — complete valid JSON response (1.7s)
- `categories` — all 9 categories, 12 lines (2.8s)
- `get <param>` — single setting value (1.3s)
- `set <param> <value>` + `get <param>` — round-trip verified (2.4s)
- NUS coexists with BleScanner (both use shared SoftDevice S140)

**Known limitations:**
- MTU stays at 23 bytes despite requesting 247 (bleak/bluez issue — works fine, just slower)
- Throughput ~5 KB/s at min MTU (4 chunks × 20 bytes × 66 Hz loop)
- `json settings` response (~6 KB) overflows 4 KB ring buffer — use `show <cat>` or individual `get` commands instead
- BSP's `getHvnPacket()` has 100ms blocking timeout — causes ~200 bytes/s post-burst throughput
- BLE address: `F4:15:6D:FA:4D:93` (device SN 06ACEB165A468CB7)

**Resource impact (nRF52840):**
- Flash: 392 KB (+47 KB from baseline, ~48% of 811 KB)
- RAM: 24.3 KB (+4.1 KB, ~10% of 237 KB)
- CPU: negligible (BLE is ISR-driven, ring buffer drain is O(1) per loop)

### Completed: BLE Advertising Protocol (Step 0, earlier work)

Already implemented before NUS work:
- `comms/BleProtocol.h` — shared packet format
- `comms/BleScanner.h/cpp` — nRF52840 passive scan + command callback
- `comms/BleAdvertiser.h/cpp` — ESP32-S3 broadcast
- `comms/WifiManager.h/cpp` — ESP32-S3 WiFi config serial commands
- `ble` and `wifi` serial commands in SerialConsole
- Both platforms compile cleanly

### Completed: blinky-server scaffold (earlier work)

Server foundation exists but transports are stubs:
- `transport/base.py` — Transport ABC (connect/disconnect/send/receive)
- `transport/serial_transport.py` — USB serial transport (working)
- `transport/ble_transport.py` — BLE transport (NotImplementedError stub)
- `transport/wifi_transport.py` — WiFi transport (NotImplementedError stub)
- `transport/discovery.py` — USB VID/PID device discovery
- `device/protocol.py` — Line-based command/response protocol (100ms timeout)
- `device/device.py` — Device class with stream fan-out
- `device/manager.py` — Fleet manager
- `api/` — REST + WebSocket API (FastAPI)

---

## Next Steps

### Step 2: BleTransport in blinky-server

Implement the BLE transport using `bleak` so the Pi can manage nRF52840 devices wirelessly with the same API as serial.

**Files to create/modify:**
- `blinky-server/blinky_server/transport/ble_transport.py` — Implement Transport interface using bleak
- `blinky-server/blinky_server/transport/discovery.py` — Add BLE NUS device scanning

**Key design decisions:**
- Use `bleak.BleakClient` for connection management
- Subscribe to NUS TXD notifications (UUID `6e400003-...`) for device output
- Write commands to NUS RXD (UUID `6e400002-...`) with `response=True` for reliability
- Line-reassembly buffer on receive side (notifications may split across MTU boundaries)
- Use `response=True` (write-with-response) to avoid bluez write failures from rapid fire-and-forget
- Add 300ms delay between commands to allow BLE stack to settle
- Longer timeouts than serial (~5s vs 100ms) due to BLE latency

**Transport interface to implement:**
```python
class BleTransport(Transport):
    async def connect(self) -> None: ...     # BleakClient connect + start_notify
    async def disconnect(self) -> None: ...  # stop_notify + disconnect
    async def send(self, data: bytes) -> None: ...  # write to RXD
    async def receive(self) -> bytes: ...    # read from notification buffer
    @property
    def is_connected(self) -> bool: ...
```

### Step 3: BLE Discovery

Add BLE device scanning to the fleet manager so it can find NUS-advertising nRF52840 devices automatically.

- Scan for devices advertising NUS service UUID `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- Match by name prefix "Blinky"
- Store BLE address for reconnection
- Integrate with existing device registry

### Step 4: Web Bluetooth in blinky-console

Add Web Bluetooth NUS support to the React web app alongside existing WebSerial.

- `bluetooth.ts` service (NUS-based, same command protocol)
- "Bluetooth" button in ConnectionBar
- Chrome Desktop + Android support

### Step 5: WiFi TCP on ESP32-S3

Add WiFi-based TCP transport for ESP32-S3 devices.

- ESP32-S3 runs a lightweight TCP server on Core 0
- Same line-based protocol as serial/BLE
- `WifiTransport` in blinky-server
- WiFi credentials already configurable via `wifi ssid/pass/connect` serial commands

### Step 6: Wireless Discovery

Unified discovery across serial, BLE, and WiFi.

- mDNS for WiFi devices (ESP32-S3 advertises `_blinky._tcp`)
- BLE scanning for NUS devices (nRF52840)
- USB VID/PID for serial (existing)

---

## Protocol Compatibility

| Protocol | nRF52840 | ESP32-S3 | Shared? |
|----------|----------|----------|---------|
| **BLE** | Yes (SoftDevice S140 v7.3.0) | Yes (ESP32 BLE library) | **Yes** |
| **802.15.4 / Thread / Zigbee** | Yes (hardware) | No (ESP32-H2/C6 only) | No |
| **ESB (Enhanced ShockBurst)** | Yes (Nordic proprietary) | No | No |
| **ESP-NOW** | No | Yes (Espressif proprietary) | No |
| **WiFi** | No | Yes | N/A |

## Resource Budget

### nRF52840 (with NUS, measured)

| Resource | Without NUS | With NUS | Available | Headroom |
|----------|-------------|----------|-----------|----------|
| **Flash** | ~345 KB | ~392 KB | 811 KB | 419 KB (52%) |
| **RAM** | ~20 KB | ~24 KB | 237 KB | 213 KB (90%) |
| **CPU** | ~84% | ~84% | - | ~16% |

### ESP32-S3 (Gateway, estimated)

| Resource | Current | Added by WiFi | Available | Headroom |
|----------|---------|---------------|-----------|----------|
| **Flash** | ~345 KB | ~170 KB | 8 MB | 7.4 MB |
| **RAM** | ~60 KB | ~50 KB | 512 KB | 400 KB |
| **Core 1 CPU** | 68% | 0% (Core 0) | - | 32% |

---

## BLE NUS Technical Details

### Architecture

```
Command from Pi (bleak)
  → BLE write to RXD characteristic
  → SoftDevice ISR → deferred Ada callback task
  → BleNus::onRxData → processRxByte → lineCallback
  → SerialConsole::handleCommand(line)
  → output via out_.print() → TeeStream
  → TeeStream writes to Serial (primary) + BleNus (secondary)
  → BleNus::write() → 4 KB ring buffer
  → BleNus::update() in main loop → drainTxBuffer()
  → BLEUart::write() → BLECharacteristic::notify()
  → SoftDevice → BLE notification to Pi
  → bleak notification callback → line reassembly
```

### Why Paced Ring Buffer?

The SoftDevice has a limited number of HVN (Handle Value Notification) TX buffers (~4-7). `BLECharacteristic::notify()` calls `getHvnPacket()` which blocks for up to 100ms when the queue is full, then aborts the entire write if it times out. This means a 130-byte response at 20-byte MTU (7 notifications) would lose data after the TX queue fills.

The ring buffer decouples command processing (synchronous, fills buffer instantly) from BLE transmission (asynchronous, paced by main loop + connection interval). The drain sends up to 4 MTU-chunks per `update()` call, allowing the SoftDevice to process TX complete events between iterations.

### BLE Advertising Protocol (separate from NUS)

ESP32-S3 broadcasts settings/commands as BLE extended advertising packets. nRF52840 devices passively scan and apply matching packets. This is fleet-wide multicast for scene/settings pushes, separate from the point-to-point NUS connection.

Packet format: manufacturer-specific data (0xFF), company ID 0xFFFF, protocol v1, packet type + sequence + fragment + JSON payload (up to ~240 bytes).

---

## Testing Checklist

### NUS (nRF52840) — VERIFIED
- [x] BLE NUS service advertises (UUID `6e400001-...`)
- [x] bleak discovers device by NUS service UUID
- [x] Connection established from Pi
- [x] `json info` — complete valid JSON over BLE
- [x] `categories` — all categories received
- [x] `get <param>` — setting value returned
- [x] `set <param> <value>` — value changed, confirmed by subsequent get
- [x] NUS coexists with BleScanner
- [x] USB serial still works normally while BLE connected
- [x] `ble nus` diagnostics show connection state
- [ ] MTU negotiation to 247 (currently stays at 23)
- [ ] `show <category>` — multi-line moderate response
- [ ] `json settings` over BLE (needs larger buffer or paginated API)

### BleTransport (blinky-server) — NOT STARTED
- [ ] Connect to NUS device via bleak
- [ ] Send command and receive response
- [ ] Line reassembly from BLE notifications
- [ ] Timeout handling for slow BLE responses
- [ ] Auto-reconnect on disconnect
- [ ] BLE device discovery (scan for NUS UUID)

### Web Bluetooth (blinky-console) — NOT STARTED
- [ ] "Bluetooth" button in ConnectionBar
- [ ] NUS connection via Web Bluetooth API
- [ ] Settings panel works over BLE
- [ ] Works on Chrome Desktop and Android

### WiFi (ESP32-S3) — NOT STARTED
- [ ] TCP server on ESP32-S3
- [ ] WifiTransport in blinky-server
- [ ] mDNS discovery

---

## Known Limitations

1. **BLE throughput** — ~5 KB/s at 23-byte MTU. Adequate for commands/settings, not for streaming.
2. **Ring buffer overflow** — Responses > 4 KB are truncated. `json settings` doesn't work over BLE; use `show <cat>` or individual `get` commands.
3. **BSP blocking** — `getHvnPacket()` blocks 100ms on TX queue full. Can't be fixed without BSP patch.
4. **MTU negotiation** — Device requests 247 but bleak/bluez stays at 23. Functional but slower than necessary.
5. **BLE range** — ~10-30m indoors. All devices must be in the same room.
6. **Single connection** — NUS supports one BLE central at a time (SoftDevice S140 peripheral mode).
