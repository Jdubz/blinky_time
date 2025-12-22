# Bluetooth Implementation Plan

*Created: December 2025*

## Goal

Add Bluetooth Low Energy (BLE) support so the blinky-console web app can communicate with the device wirelessly. The same JSON protocol works over both Serial and Bluetooth.

## MVP Scope

**In Scope:**
- BLE connection using Nordic UART Service (NUS)
- Same commands work over BLE as Serial
- User can choose Serial or Bluetooth when connecting

**Out of Scope (for now):**
- Simultaneous Serial + Bluetooth connections
- Auto-reconnection
- Connection quality indicators
- Transport abstraction layers

## Why Nordic UART Service?

NUS emulates a serial port over BLE. This means:
- **Zero protocol changes** - same JSON commands work
- **Minimal Arduino changes** - just add a second "serial-like" output
- **Simple web implementation** - read/write characteristics like a stream

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

## Future Improvements (Post-MVP)

- Refactor shared code between SerialConsole and BLEConsole
- Auto-reconnect to last-used device
- Show connection type in UI (USB vs Bluetooth icon)
- BLE signal strength indicator

---

*This MVP plan prioritizes getting Bluetooth working with minimal changes. Abstractions and polish can come later.*
