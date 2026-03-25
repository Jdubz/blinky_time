# Wireless Communication Plan

*Created: December 2025, Updated: March 2026*

## Goal

Add wireless communication so devices can be managed remotely as a fleet. ESP32-S3 devices connect to a server via WiFi and relay commands to nRF52840 devices via BLE. The blinky-console web app can also connect directly to any device via BLE.

## Architecture Overview

```
Server (WiFi)
  ↕ WiFi (TCP/WebSocket)
ESP32-S3 devices (gateway role, dual-core)
  ↕ BLE (Nordic UART Service)
nRF52840 devices (peripheral role)
```

**Use case**: Fleet management — settings changes, mode/scene selection, device configuration. No real-time streaming or time-dependent data over wireless.

### Why This Topology?

- ESP32-S3 has WiFi + BLE coexistence (hardware radio time-multiplexing, standard ESP-IDF feature)
- ESP32-S3 is dual-core: Core 0 handles all comms, Core 1 runs audio+LED loop untouched at 60fps
- nRF52840 has mature BLE stack (SoftDevice S140 + Bluefruit52Lib, pre-installed)
- BLE is the only shared radio protocol between the two chips (see Protocol Compatibility below)

### Protocol Compatibility

| Protocol | nRF52840 | ESP32-S3 | Shared? |
|----------|----------|----------|---------|
| **BLE** | Yes (SoftDevice S140 v7.3.0) | Yes (ESP32 BLE library) | **Yes** |
| **802.15.4 / Thread / Zigbee** | Yes (hardware) | No (ESP32-H2/C6 only) | No |
| **ESB (Enhanced ShockBurst)** | Yes (Nordic proprietary) | No | No |
| **ESP-NOW** | No | Yes (Espressif proprietary) | No |
| **WiFi** | No | Yes | N/A |

BLE is the only option without adding new hardware. For the use case (infrequent settings/scene pushes), BLE is more than sufficient.

## Resource Budget

### ESP32-S3 (Gateway)

| Resource | Current Usage | Added by Wireless | Available | Headroom |
|----------|---------------|-------------------|-----------|----------|
| **Flash** | ~345 KB | ~170 KB (WiFi ~100 KB + BLE ~60 KB + fleet ~10 KB) | 8 MB | 7.4 MB |
| **RAM** | ~60 KB | ~50 KB (WiFi ~30 KB + BLE ~15 KB + fleet ~5 KB) | 512 KB | 400 KB |
| **Core 1 CPU** | 11.3/16.7ms (68%) | 0ms (all comms on Core 0) | - | **5.4ms** |
| **Core 0 CPU** | idle | WiFi + BLE + fleet tasks | - | abundant |

### nRF52840 (Peripheral)

| Resource | Current Usage | Added by BLE | Available | Headroom |
|----------|---------------|--------------|-----------|----------|
| **Flash** | ~345 KB | ~35 KB (Bluefruit52Lib + SoftDevice) | 1 MB | 560 KB |
| **RAM** | ~60 KB | ~10 KB (SoftDevice ~8 KB + BLEUart ~2 KB) | 256 KB | 170 KB |
| **CPU** | 14/16.7ms (84%) | <0.3ms (ISR-driven, no main loop impact) | - | **2.4ms** |

**60fps is safe on both platforms.** ESP32-S3 render loop is completely unaffected (Core 0 isolation). nRF52840 BLE is interrupt-driven by the SoftDevice — the main loop doesn't notice.

## ESP32-S3 Dual-Core Architecture

```
Core 1 (pinned — unchanged firmware)        Core 0 (new — comms & fleet)
─────────────────────────────────            ──────────────────────────────
PDM mic → FFT → NN → AudioTracker           WiFi client task
  → Generator → Effect → LEDs                 ↕ server connection (WebSocket)
  → SerialConsole (USB debug)                BLE central task
  60fps, 11.3ms/frame                          ↕ connect to nRF52840 devices
                                             Fleet manager task
                                               ↕ scene state, device registry

         Shared: settings struct (mutex-protected FreeRTOS queue)
```

- WiFi + BLE stacks already run their internal event loops on Core 0 by default in ESP32 Arduino
- Communication between cores via mutex-protected settings struct or FreeRTOS queue
- Core 1 reads shared state each frame; Core 0 writes when commands arrive from server

## BLE Communication Strategy

For infrequent settings and scene data, two viable approaches:

### Option A: Brief GATT Connections (recommended for reliability)

- ESP32-S3 connects to nRF52840 via NUS, pushes settings blob, disconnects
- Whole interaction: <500ms per device
- Fleet of 3 nRF52840 devices: <2s for full fleet update (sequential)
- Between connections: zero CPU overhead on nRF52840
- Guaranteed delivery (GATT provides acknowledgment)

### Option B: BLE Advertising Broadcast (lightest possible)

- ESP32-S3 encodes settings in BLE extended advertising packets (up to 255 bytes, BLE 5)
- nRF52840 devices passively scan, pick up broadcasts matching a known manufacturer ID
- No connection, no handshake, no pairing — free multicast to all devices
- For larger scene data: fragment across multiple packets with sequence numbers
- Trade-off: no delivery guarantee (fire-and-forget)

**Recommendation**: Option A for settings/scene pushes (need confirmation), Option B for optional status beacons from nRF52840 devices.

## Scope

### Phase 1: Direct BLE (MVP)

**In Scope:**
- nRF52840 BLE peripheral with NUS (same JSON commands as Serial)
- Web Bluetooth connection from blinky-console to any device
- User chooses Serial or Bluetooth when connecting

**Out of Scope:**
- WiFi, server communication, fleet management
- Auto-reconnection
- Simultaneous Serial + Bluetooth from web app

### Phase 2: WiFi Gateway + Fleet Management

**In Scope:**
- ESP32-S3 WiFi client connecting to server
- ESP32-S3 BLE central connecting to nRF52840 peripherals
- Command relay: server → ESP32-S3 → nRF52840
- Scene/settings distribution across fleet
- Device registry and status reporting

**Out of Scope:**
- OTA firmware updates over wireless
- Real-time audio streaming over wireless
- Mesh networking between nRF52840 devices

## Why Nordic UART Service?

NUS emulates a serial port over BLE. This means:
- **Zero protocol changes** - same JSON commands work
- **Minimal Arduino changes** - just add a second "serial-like" output
- **Simple web implementation** - read/write characteristics like a stream
- **ESP32-S3 central support** - ESP32 BLE library has `BLEClient` for connecting to NUS peripherals

---

## Part 1: Arduino Implementation

### Approach

Keep `SerialConsole` as-is. Create a parallel `BLEConsole` class that handles the same commands but over BLE. Both share the same `SettingsRegistry` and device state.

### New Files

```
blinky-things/
└── inputs/
    ├── BLEConsole.h
    └── BLEConsole.cpp
```

### BLEConsole.h

```cpp
#pragma once

#include <Arduino.h>
#include <bluefruit.h>
#include "../generators/Fire.h"
#include "../config/SettingsRegistry.h"

class ConfigStorage;
class Fire;
class AdaptiveMic;
class BatteryMonitor;

/**
 * BLEConsole - Bluetooth version of SerialConsole
 * Uses Nordic UART Service (NUS) for serial-like communication
 */
class BLEConsole {
public:
    BLEConsole(Fire* fireGen, AdaptiveMic* mic);

    void begin();
    void update();

    void setConfigStorage(ConfigStorage* storage) { configStorage_ = storage; }
    void setBatteryMonitor(BatteryMonitor* battery) { battery_ = battery; }
    SettingsRegistry& getSettings() { return settings_; }

    bool isConnected() const { return connected_; }

private:
    void setupBLE();
    void registerSettings();
    void handleCommand(const char* cmd);
    bool handleSpecialCommand(const char* cmd);
    void restoreDefaults();
    void streamTick();

    // BLE output helpers
    void blePrint(const char* str);
    void blePrint(const __FlashStringHelper* str);
    void blePrintln(const char* str);
    void blePrintln(const __FlashStringHelper* str);
    void blePrint(float value, int decimals);
    void blePrint(int value);

    // Callbacks
    static void connectCallback(uint16_t conn_handle);
    static void disconnectCallback(uint16_t conn_handle, uint8_t reason);
    static void rxCallback(uint16_t conn_handle);

    // Members
    Fire* fireGenerator_;
    AdaptiveMic* mic_;
    BatteryMonitor* battery_;
    ConfigStorage* configStorage_;
    SettingsRegistry settings_;

    BLEUart bleuart_;
    bool connected_ = false;

    // Command buffer
    char cmdBuffer_[128];
    size_t cmdPos_ = 0;

    // Streaming
    bool streamEnabled_ = false;
    uint32_t streamLastMs_ = 0;
    uint32_t batteryLastMs_ = 0;
    static const uint16_t STREAM_PERIOD_MS = 50;
    static const uint16_t BATTERY_PERIOD_MS = 1000;

    static BLEConsole* instance_;
};
```

### BLEConsole.cpp (key parts)

```cpp
#include "BLEConsole.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../types/Version.h"

extern const DeviceConfig& config;

BLEConsole* BLEConsole::instance_ = nullptr;

BLEConsole::BLEConsole(Fire* fireGen, AdaptiveMic* mic)
    : fireGenerator_(fireGen), mic_(mic), battery_(nullptr), configStorage_(nullptr) {
    instance_ = this;
}

void BLEConsole::begin() {
    setupBLE();
    settings_.begin();
    registerSettings();
}

void BLEConsole::setupBLE() {
    Bluefruit.begin();
    Bluefruit.setName("Blinky");
    Bluefruit.setTxPower(4);

    Bluefruit.Periph.setConnectCallback(connectCallback);
    Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

    bleuart_.begin();
    bleuart_.setRxCallback(rxCallback);

    // Advertising
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(bleuart_);
    Bluefruit.Advertising.addName();
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.start(0);
}

void BLEConsole::connectCallback(uint16_t conn_handle) {
    if (instance_) {
        instance_->connected_ = true;
    }
}

void BLEConsole::disconnectCallback(uint16_t conn_handle, uint8_t reason) {
    if (instance_) {
        instance_->connected_ = false;
        instance_->streamEnabled_ = false;
    }
}

void BLEConsole::rxCallback(uint16_t conn_handle) {
    if (!instance_) return;

    while (instance_->bleuart_.available()) {
        char c = instance_->bleuart_.read();

        if (c == '\n' || c == '\r') {
            if (instance_->cmdPos_ > 0) {
                instance_->cmdBuffer_[instance_->cmdPos_] = '\0';
                instance_->handleCommand(instance_->cmdBuffer_);
                instance_->cmdPos_ = 0;
            }
        } else if (instance_->cmdPos_ < sizeof(instance_->cmdBuffer_) - 1) {
            instance_->cmdBuffer_[instance_->cmdPos_++] = c;
        }
    }
}

void BLEConsole::update() {
    if (!connected_) return;
    streamTick();
}

// BLE output - same pattern as Serial but to bleuart_
void BLEConsole::blePrint(const char* str) {
    bleuart_.print(str);
}

void BLEConsole::blePrintln(const char* str) {
    bleuart_.println(str);
}

// ... handleCommand, handleSpecialCommand, streamTick are identical to SerialConsole
// but use blePrint/blePrintln instead of Serial.print/Serial.println
```

### Main Sketch Changes

```cpp
// blinky-things.ino

#include "inputs/SerialConsole.h"
#include "inputs/BLEConsole.h"

SerialConsole serialConsole(&fireGenerator, &adaptiveMic);
BLEConsole bleConsole(&fireGenerator, &adaptiveMic);

void setup() {
    // ... existing setup ...

    serialConsole.begin();
    bleConsole.begin();

    // Both share the same config storage and battery monitor
    serialConsole.setConfigStorage(&configStorage);
    serialConsole.setBatteryMonitor(&batteryMonitor);
    bleConsole.setConfigStorage(&configStorage);
    bleConsole.setBatteryMonitor(&batteryMonitor);
}

void loop() {
    // ... existing loop code ...

    serialConsole.update();
    bleConsole.update();
}
```

### Code Duplication Note

`BLEConsole` will have similar code to `SerialConsole`. For MVP, this duplication is acceptable. If we find ourselves making identical changes to both files, we can refactor later. Premature abstraction would slow us down.

---

## Part 2: Web Console Implementation

### Approach

Add a `bluetooth.ts` service alongside `serial.ts`. Update `useSerial` hook to support both connection types. Minimal changes to components.

### New File: bluetooth.ts

```typescript
// services/bluetooth.ts

import { AudioMessage, BatteryMessage, DeviceInfo, SettingsResponse } from '../types';

const NUS_SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX_CHAR_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_RX_CHAR_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

export type BluetoothEventType = 'connected' | 'disconnected' | 'data' | 'error' | 'audio' | 'battery';

export interface BluetoothEvent {
  type: BluetoothEventType;
  data?: string;
  audio?: AudioMessage;
  battery?: BatteryMessage;
  error?: Error;
}

export type BluetoothEventCallback = (event: BluetoothEvent) => void;

class BluetoothService {
  private device: BluetoothDevice | null = null;
  private txChar: BluetoothRemoteGATTCharacteristic | null = null;
  private rxChar: BluetoothRemoteGATTCharacteristic | null = null;
  private listeners: BluetoothEventCallback[] = [];
  private buffer: string = '';

  isSupported(): boolean {
    return 'bluetooth' in navigator;
  }

  addEventListener(callback: BluetoothEventCallback): void {
    this.listeners.push(callback);
  }

  removeEventListener(callback: BluetoothEventCallback): void {
    this.listeners = this.listeners.filter(l => l !== callback);
  }

  private emit(event: BluetoothEvent): void {
    this.listeners.forEach(cb => cb(event));
  }

  async connect(): Promise<boolean> {
    try {
      this.device = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: 'Blinky' }],
        optionalServices: [NUS_SERVICE_UUID]
      });

      this.device.addEventListener('gattserverdisconnected', () => {
        this.cleanup();
        this.emit({ type: 'disconnected' });
      });

      const server = await this.device.gatt?.connect();
      if (!server) throw new Error('Failed to connect');

      const service = await server.getPrimaryService(NUS_SERVICE_UUID);
      this.txChar = await service.getCharacteristic(NUS_TX_CHAR_UUID);
      this.rxChar = await service.getCharacteristic(NUS_RX_CHAR_UUID);

      await this.rxChar.startNotifications();
      this.rxChar.addEventListener('characteristicvaluechanged', this.onData.bind(this));

      this.emit({ type: 'connected' });
      return true;
    } catch (error) {
      this.emit({ type: 'error', error: error as Error });
      return false;
    }
  }

  async disconnect(): Promise<void> {
    if (this.device?.gatt?.connected) {
      this.device.gatt.disconnect();
    }
    this.cleanup();
    this.emit({ type: 'disconnected' });
  }

  private cleanup(): void {
    this.device = null;
    this.txChar = null;
    this.rxChar = null;
    this.buffer = '';
  }

  isConnected(): boolean {
    return this.device?.gatt?.connected ?? false;
  }

  async send(command: string): Promise<void> {
    if (!this.txChar) throw new Error('Not connected');

    const data = new TextEncoder().encode(command.trim() + '\n');
    await this.txChar.writeValueWithoutResponse(data);
  }

  async sendAndReceiveJson<T>(command: string, timeoutMs = 2000): Promise<T | null> {
    return new Promise(resolve => {
      let resolved = false;
      let jsonBuffer = '';

      const timeout = setTimeout(() => {
        if (!resolved) {
          resolved = true;
          this.removeEventListener(handler);
          resolve(null);
        }
      }, timeoutMs);

      const handler = (event: BluetoothEvent) => {
        if (event.type === 'data' && event.data) {
          jsonBuffer += event.data;
          const lines = jsonBuffer.split('\n');
          for (const line of lines) {
            const trimmed = line.trim();
            if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
              try {
                const parsed = JSON.parse(trimmed) as T;
                if (!resolved) {
                  resolved = true;
                  clearTimeout(timeout);
                  this.removeEventListener(handler);
                  resolve(parsed);
                }
                return;
              } catch { /* ignore */ }
            }
          }
        }
      };

      this.addEventListener(handler);
      this.send(command).catch(() => {
        if (!resolved) {
          resolved = true;
          clearTimeout(timeout);
          this.removeEventListener(handler);
          resolve(null);
        }
      });
    });
  }

  getDeviceInfo(): Promise<DeviceInfo | null> {
    return this.sendAndReceiveJson<DeviceInfo>('json info');
  }

  getSettings(): Promise<SettingsResponse | null> {
    return this.sendAndReceiveJson<SettingsResponse>('json settings');
  }

  async setSetting(name: string, value: number | boolean): Promise<void> {
    await this.send(`set ${name} ${value}`);
  }

  async setStreamEnabled(enabled: boolean): Promise<void> {
    await this.send(enabled ? 'stream on' : 'stream off');
  }

  async saveSettings(): Promise<void> { await this.send('save'); }
  async loadSettings(): Promise<void> { await this.send('load'); }
  async resetDefaults(): Promise<void> { await this.send('defaults'); }

  private onData(event: Event): void {
    const char = event.target as BluetoothRemoteGATTCharacteristic;
    if (!char.value) return;

    const text = new TextDecoder().decode(char.value);
    this.buffer += text;

    const lines = this.buffer.split('\n');
    this.buffer = lines.pop() || '';

    for (const line of lines) {
      const trimmed = line.trim();
      if (!trimmed) continue;

      if (trimmed.startsWith('{"a":')) {
        try {
          this.emit({ type: 'audio', audio: JSON.parse(trimmed) });
          continue;
        } catch { /* ignore */ }
      }

      if (trimmed.startsWith('{"b":')) {
        try {
          this.emit({ type: 'battery', battery: JSON.parse(trimmed) });
          continue;
        } catch { /* ignore */ }
      }

      this.emit({ type: 'data', data: trimmed });
    }
  }
}

export const bluetoothService = new BluetoothService();
```

### Update useSerial Hook

Add a `connectionType` state and methods for both Serial and Bluetooth:

```typescript
// hooks/useSerial.ts (additions)

import { bluetoothService } from '../services/bluetooth';

// Add to hook state:
const [connectionType, setConnectionType] = useState<'serial' | 'bluetooth' | null>(null);

// Add to return:
isBluetoothSupported: bluetoothService.isSupported(),
connectionType,

// Add connect method for Bluetooth:
const connectBluetooth = useCallback(async () => {
  setConnectionState('connecting');

  const success = await bluetoothService.connect();
  if (!success) {
    setConnectionState('error');
    return;
  }

  setConnectionType('bluetooth');

  // Set up event listener (same pattern as serial)
  bluetoothService.addEventListener(handleEvent);

  // Fetch device info and settings
  const info = await bluetoothService.getDeviceInfo();
  const settingsResp = await bluetoothService.getSettings();

  if (info && settingsResp) {
    setDeviceInfo(info);
    setSettings(settingsResp.settings);
    setConnectionState('connected');
  } else {
    setConnectionState('error');
  }
}, [handleEvent]);

// Update disconnect to handle both:
const disconnect = useCallback(async () => {
  if (connectionType === 'serial') {
    await serialService.disconnect();
  } else if (connectionType === 'bluetooth') {
    bluetoothService.removeEventListener(handleEvent);
    await bluetoothService.disconnect();
  }
  setConnectionType(null);
}, [connectionType, handleEvent]);

// Update other methods to use correct service based on connectionType
```

### Update ConnectionBar

Add Bluetooth connect button:

```tsx
{!isConnected && !isConnecting && (
  <>
    {isSupported && (
      <button className="btn btn-connect" onClick={onConnect}>
        USB
      </button>
    )}
    {isBluetoothSupported && (
      <button className="btn btn-connect" onClick={onConnectBluetooth}>
        Bluetooth
      </button>
    )}
  </>
)}
```

---

## Implementation Order

### Week 1: Arduino BLE

1. Create `BLEConsole.h/cpp` (copy structure from `SerialConsole`)
2. Add BLE setup and advertising
3. Implement command receive via NUS RX callback
4. Implement output via NUS TX
5. Add streaming support
6. Test with nRF Connect app

### Week 2: Web Console

1. Create `bluetooth.ts` service
2. Add `connectBluetooth` to `useSerial` hook
3. Update `ConnectionBar` with second button
4. Route commands through correct service based on connection type
5. Test on Chrome desktop and Android

---

## Testing Checklist

### Arduino
- [ ] Device advertises as "Blinky" in nRF Connect
- [ ] Can connect from nRF Connect
- [ ] `json info` returns device info over BLE
- [ ] `json settings` returns all settings over BLE
- [ ] `set <name> <value>` works over BLE
- [ ] `stream on` sends audio/battery data over BLE
- [ ] Serial still works when BLE is connected

### Web Console
- [ ] "Bluetooth" button appears in ConnectionBar
- [ ] Browser shows device picker when clicking Bluetooth
- [ ] Device info loads after BLE connection
- [ ] Settings panel works over BLE
- [ ] Audio streaming works over BLE
- [ ] Works on Chrome Android

---

## Known Limitations (MVP)

1. **No simultaneous connections** - Can't use Serial and Bluetooth at same time from web app (device supports both, but web app only tracks one)
2. **No auto-reconnect** - Must manually reconnect if BLE disconnects
3. **Code duplication** - `SerialConsole` and `BLEConsole` have similar code. Acceptable for MVP.
4. **Settings shared** - Both consoles share the same `SettingsRegistry`. Changing a setting via Serial also changes it for BLE (this is actually correct behavior).

---

## Part 3: ESP32-S3 WiFi Gateway (Phase 2)

### Approach

All networking runs on Core 0 via FreeRTOS tasks. Core 1 (audio+LED loop) is untouched. Communication between cores uses a mutex-protected shared settings struct.

### New Files

```
blinky-things/
└── comms/
    ├── WifiGateway.h        # WiFi client + server connection
    ├── WifiGateway.cpp
    ├── BLECentral.h         # BLE central, connects to nRF52840 devices
    ├── BLECentral.cpp
    ├── FleetManager.h       # Device registry, command routing
    ├── FleetManager.cpp
    └── SharedState.h        # Mutex-protected state shared between cores
```

### Core 0 Task Structure

```cpp
// Created in setup(), pinned to Core 0
xTaskCreatePinnedToCore(wifiTask, "wifi", 4096, NULL, 1, NULL, 0);
xTaskCreatePinnedToCore(bleGatewayTask, "ble_gw", 4096, NULL, 1, NULL, 0);
xTaskCreatePinnedToCore(fleetTask, "fleet", 2048, NULL, 1, NULL, 0);
```

### SharedState.h (inter-core communication)

```cpp
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct SharedState {
    SemaphoreHandle_t mutex;

    // Written by Core 0 (comms), read by Core 1 (render loop)
    uint8_t pendingGenerator;       // generator change request
    bool settingsDirty;             // flag: new settings available
    char pendingCommand[128];       // queued command from server
    bool commandPending;

    // Written by Core 1 (render loop), read by Core 0 (comms)
    uint8_t currentGenerator;
    float energy;                   // for status reporting
    float bpm;

    void init() {
        mutex = xSemaphoreCreateMutex();
        commandPending = false;
        settingsDirty = false;
    }

    bool lock(TickType_t timeout = pdMS_TO_TICKS(10)) {
        return xSemaphoreTake(mutex, timeout) == pdTRUE;
    }

    void unlock() {
        xSemaphoreGive(mutex);
    }
};
```

### BLE Central (ESP32-S3 connecting to nRF52840 devices)

```cpp
// Uses ESP32 BLE library in Central (client) mode
#include <BLEDevice.h>
#include <BLEClient.h>

// Connect to nRF52840 NUS peripheral, send settings, disconnect
bool pushSettingsToDevice(BLEAddress address, const char* settingsJson) {
    BLEClient* client = BLEDevice::createClient();
    if (!client->connect(address)) return false;

    BLERemoteService* nus = client->getService(NUS_SERVICE_UUID);
    if (!nus) { client->disconnect(); return false; }

    BLERemoteCharacteristic* rx = nus->getCharacteristic(NUS_RX_UUID);
    if (!rx) { client->disconnect(); return false; }

    rx->writeValue((uint8_t*)settingsJson, strlen(settingsJson));
    client->disconnect();
    return true;
}
```

### WiFi Gateway

```cpp
// Connects to server, receives commands, routes to local or BLE devices
void wifiTask(void* param) {
    WiFi.begin(ssid, password);
    // ... connect to server via WebSocket ...

    while (true) {
        // Receive command from server
        // Parse target device
        // If local: write to SharedState
        // If remote nRF52840: queue for BLE central task
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

### Platform Guard

WiFi gateway code only compiles on ESP32-S3:

```cpp
#ifdef BLINKY_PLATFORM_ESP32S3
    #include "comms/WifiGateway.h"
    #include "comms/BLECentral.h"
    #include "comms/FleetManager.h"
#endif
```

nRF52840 only gets the BLE peripheral (BLEConsole from Phase 1).

---

## Implementation Order (Revised)

### Phase 1: nRF52840 BLE Peripheral (MVP)

1. Create `BLEConsole.h/cpp` for nRF52840 (NUS peripheral)
2. Test with nRF Connect app
3. Add `bluetooth.ts` to blinky-console web app
4. Test direct Web Bluetooth connections

### Phase 2: ESP32-S3 WiFi + BLE Gateway

1. Add FreeRTOS tasks on Core 0 for WiFi + BLE central
2. Implement `SharedState` for inter-core communication
3. Implement BLE central (connect to nRF52840, push settings)
4. Implement WiFi client (connect to server)
5. Implement `FleetManager` (device registry, command routing)
6. Build server (scope TBD)

---

## Testing Checklist

### Phase 1: Arduino BLE
- [ ] nRF52840 advertises as "Blinky-{deviceId}" in nRF Connect
- [ ] Can connect from nRF Connect
- [ ] `json info` returns device info over BLE
- [ ] `json settings` returns all settings over BLE
- [ ] `set <name> <value>` works over BLE
- [ ] `stream on` sends audio/battery data over BLE
- [ ] Serial still works when BLE is connected
- [ ] 60fps maintained with BLE advertising + connection

### Phase 1: Web Console
- [ ] "Bluetooth" button appears in ConnectionBar
- [ ] Browser shows device picker when clicking Bluetooth
- [ ] Device info loads after BLE connection
- [ ] Settings panel works over BLE
- [ ] Audio streaming works over BLE
- [ ] Works on Chrome Android

### Phase 2: ESP32-S3 Gateway
- [ ] ESP32-S3 connects to WiFi and server
- [ ] ESP32-S3 discovers nRF52840 BLE peripherals
- [ ] ESP32-S3 pushes settings to nRF52840 via BLE
- [ ] Server commands reach nRF52840 via ESP32-S3 relay
- [ ] Core 1 frame rate unaffected (60fps) during WiFi+BLE activity
- [ ] Fleet-wide scene change propagates to all devices

---

## Known Limitations

1. **No simultaneous web connections** - Web app tracks one connection (Serial or Bluetooth, not both)
2. **No auto-reconnect** - Must manually reconnect if BLE disconnects
3. **Code duplication** - `SerialConsole` and `BLEConsole` have similar code. Acceptable for Phase 1.
4. **Settings shared** - Both consoles share the same `SettingsRegistry`. Changing a setting via Serial also changes it for BLE (this is correct behavior).
5. **BLE range** - ~10-30m indoors. nRF52840 and ESP32-S3 must be in the same room.
6. **Relay latency** - Server → ESP32 → BLE → nRF52840 adds ~50-100ms. Fine for settings, not for sync.
7. **BLE connections from ESP32-S3** - ESP32-S3 can maintain 3-4 simultaneous BLE central connections while running WiFi.

---

## Future Improvements (Post-Phase 2)

- Refactor shared code between SerialConsole and BLEConsole into transport abstraction
- Auto-reconnect to last-used device
- Show connection type in UI (USB vs Bluetooth icon)
- BLE signal strength indicator
- OTA firmware updates via WiFi (ESP32-S3) or BLE relay (nRF52840)
- BLE advertising broadcast for status beacons (Option B from BLE strategy)
- Scene synchronization across devices (coordinated transitions)
