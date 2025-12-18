# Blinky Console UI - MVP Plan

## Goal
Simple web app to visualize audio and control device parameters. Ship fast, iterate later.

---

## Part 1: Firmware Changes

Add JSON output mode for reliable parsing. Three new commands:

### 1.1 `json settings` - Dump all settings as JSON
```json
{"settings":[
  {"name":"cooling","value":120,"type":"uint8","cat":"fire","min":0,"max":255},
  {"name":"sparkchance","value":0.35,"type":"float","cat":"fire","min":0,"max":1},
  {"name":"gate","value":0.04,"type":"float","cat":"audio","min":0,"max":1}
]}
```

### 1.2 `json info` - Device info
```json
{"device":"BucketTotem","version":"0.3.0","width":4,"height":15,"leds":60}
```

### 1.3 `stream on/off` - Continuous JSON audio data (~20Hz)
```json
{"a":{"l":0.45,"t":0.85,"e":0.32,"g":3.5}}
```
Short keys to minimize serial bandwidth: `l`=level, `t`=transient, `e`=envelope, `g`=gain

---

## Part 2: UI App

### Stack
- **WebSerial API** (browser-native serial port access)
- **TypeScript** (type safety)
- **Vanilla HTML/CSS** (no framework)
- **Chart.js** with streaming plugin (audio timeline)
- **Firebase Hosting** (free tier, CDN, HTTPS)
- **PWA** (installable, works offline)

### Browser Support
- ✅ Chrome 89+
- ✅ Edge 89+
- ✅ Opera 76+
- ❌ Firefox (in development)
- ❌ Safari (not planned)

### MVP Features (v0.1)

1. **Connection**
   - Port dropdown + Connect button
   - Auto-fetch `json info` on connect
   - Permission persists per-origin

2. **Settings Panel**
   - Fetch with `json settings`
   - Render sliders/toggles dynamically
   - Send `set <name> <value>` on change (debounced)

3. **Audio Visualizer**
   - Enable with `stream on`
   - Scrolling line chart (last 30 seconds)
   - Show level + transient markers

4. **Console**
   - Simple log of serial output
   - Text input to send commands

### Not in MVP
- Presets
- Themes
- Multi-device
- Fancy animations
- Offline mode (PWA caching)

---

## Part 3: File Structure

```
blinky-console/
├── package.json
├── tsconfig.json
├── firebase.json          # Firebase hosting config
├── .firebaserc            # Firebase project config
├── public/
│   ├── index.html
│   ├── manifest.json      # PWA manifest
│   └── favicon.ico
├── src/
│   ├── main.ts            # App entry point
│   ├── serial.ts          # WebSerial wrapper
│   ├── ui.ts              # UI rendering
│   ├── chart.ts           # Audio visualization
│   ├── styles.css
│   └── types.ts           # Shared interfaces
└── dist/                  # Compiled output (deployed to Firebase)
```

---

## Part 4: Implementation Order

### Step 1: Firmware (~1 hour)
- [ ] Add `printSettingsJson()` to existing SettingsRegistry
- [ ] Add `json info` command to existing SerialConsole
- [ ] Add `stream on/off` + `streamTick()` to SerialConsole
- [ ] Test with serial monitor

### Step 2: WebSerial Shell (~1 hour)
- [ ] Basic HTML + TypeScript + Vite setup
- [ ] WebSerial connect/disconnect
- [ ] Raw console log display
- [ ] Command input

### Step 3: Settings Panel (~2 hours)
- [ ] Parse `json settings` response
- [ ] Generate controls (sliders for numbers, toggles for bools)
- [ ] Wire up `set` commands with debouncing

### Step 4: Audio Visualizer (~2 hours)
- [ ] Parse streaming JSON
- [ ] Chart.js with realtime plugin
- [ ] Level line + transient markers

### Step 5: Polish + Deploy (~1 hour)
- [ ] Error handling & browser compatibility message
- [ ] Connection status indicator
- [ ] Basic styling
- [ ] Firebase hosting setup & deploy

**Total: ~7-8 hours**

---

## Part 5: TypeScript Types

```typescript
// src/types.ts

export interface DeviceInfo {
  device: string;
  version: string;
  width: number;
  height: number;
  leds: number;
}

export type SettingType = 'uint8' | 'int8' | 'uint16' | 'uint32' | 'float' | 'bool';

export interface DeviceSetting {
  name: string;
  value: number | boolean;
  type: SettingType;
  cat: string;
  min: number;
  max: number;
}

export interface SettingsResponse {
  settings: DeviceSetting[];
}

export interface AudioSample {
  l: number;  // level
  t: number;  // transient
  e: number;  // envelope
  g: number;  // gain
}

export interface AudioMessage {
  a: AudioSample;
}
```

---

## Part 6: Firmware Implementation

> **Note:** SettingsRegistry and SerialConsole already exist in the codebase.
> These changes add JSON output to the existing infrastructure.

### SettingsRegistry.h - Add to public section
```cpp
void printSettingsJson();
static const char* typeString(SettingType t);
```

### SettingsRegistry.cpp - Add implementation
```cpp
void SettingsRegistry::printSettingsJson() {
    Serial.print(F("{\"settings\":["));
    for (uint8_t i = 0; i < numSettings_; i++) {
        if (i > 0) Serial.print(',');
        Serial.print(F("{\"name\":\""));
        Serial.print(settings_[i].name);
        Serial.print(F("\",\"value\":"));

        switch (settings_[i].type) {
            case SettingType::UINT8:
                Serial.print(*((uint8_t*)settings_[i].valuePtr));
                break;
            case SettingType::INT8:
                Serial.print(*((int8_t*)settings_[i].valuePtr));
                break;
            case SettingType::UINT16:
                Serial.print(*((uint16_t*)settings_[i].valuePtr));
                break;
            case SettingType::UINT32:
                Serial.print(*((uint32_t*)settings_[i].valuePtr));
                break;
            case SettingType::FLOAT:
                Serial.print(*((float*)settings_[i].valuePtr), 3);
                break;
            case SettingType::BOOL:
                Serial.print(*((bool*)settings_[i].valuePtr) ? "true" : "false");
                break;
        }

        Serial.print(F(",\"type\":\""));
        Serial.print(typeString(settings_[i].type));
        Serial.print(F("\",\"cat\":\""));
        Serial.print(settings_[i].category);
        Serial.print(F("\",\"min\":"));
        Serial.print(settings_[i].minVal);
        Serial.print(F(",\"max\":"));
        Serial.print(settings_[i].maxVal);
        Serial.print(F("}"));
    }
    Serial.println(F("]}"));
}

const char* SettingsRegistry::typeString(SettingType t) {
    switch (t) {
        case SettingType::UINT8: return "uint8";
        case SettingType::INT8: return "int8";
        case SettingType::UINT16: return "uint16";
        case SettingType::UINT32: return "uint32";
        case SettingType::FLOAT: return "float";
        case SettingType::BOOL: return "bool";
        default: return "unknown";
    }
}
```

### SerialConsole.h - Add to private section
```cpp
bool streamEnabled_ = false;
uint32_t streamLastMs_ = 0;
static const uint16_t STREAM_PERIOD_MS = 50;  // ~20Hz

void streamTick();
```

### SerialConsole.cpp - Add to handleSpecialCommand()
```cpp
if (strcmp(cmd, "json settings") == 0) {
    settings_.printSettingsJson();
    return true;
}

if (strcmp(cmd, "json info") == 0) {
    Serial.print(F("{\"device\":\""));
    Serial.print(config.deviceName);
    Serial.print(F("\",\"version\":\""));
    Serial.print(BLINKY_VERSION);
    Serial.print(F("\",\"width\":"));
    Serial.print(config.matrix.width);
    Serial.print(F(",\"height\":"));
    Serial.print(config.matrix.height);
    Serial.print(F(",\"leds\":"));
    Serial.print(config.matrix.width * config.matrix.height);
    Serial.println(F("}"));
    return true;
}

if (strcmp(cmd, "stream on") == 0) {
    streamEnabled_ = true;
    Serial.println(F("OK"));
    return true;
}

if (strcmp(cmd, "stream off") == 0) {
    streamEnabled_ = false;
    Serial.println(F("OK"));
    return true;
}
```

### SerialConsole.cpp - Add streamTick method
```cpp
void SerialConsole::streamTick() {
    if (!streamEnabled_ || !mic_) return;

    uint32_t now = millis();
    if (now - streamLastMs_ < STREAM_PERIOD_MS) return;
    streamLastMs_ = now;

    Serial.print(F("{\"a\":{\"l\":"));
    Serial.print(mic_->getLevel(), 2);
    Serial.print(F(",\"t\":"));
    Serial.print(mic_->getTransient(), 2);
    Serial.print(F(",\"e\":"));
    Serial.print(mic_->getEnv(), 2);
    Serial.print(F(",\"g\":"));
    Serial.print(mic_->getGlobalGain(), 1);
    Serial.println(F("}}"));
}
```

### SerialConsole.cpp - Call from update()
```cpp
void SerialConsole::update() {
    // ... existing command handling ...
    streamTick();  // Add at end of update()
}
```

---

## UI Mockup

```
┌─────────────────────────────────────────────────────────────┐
│  🔥 Blinky Console                        [Connect] [━━━━]  │
│  Tube Light v0.3.0 • 60 LEDs                    ● Connected │
├─────────────────────────────────────────────────────────────┤
│  AUDIO MONITOR                              [Stream ●]      │
│  ████████████░░░░░░░░░░░░░░░░░░░░░░░░░████████░░░░░░░░░░░  │
│  ──────────────────────────────────────────────────────────  │
│  -30s                                                  now  │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  FIRE                          │  AUDIO                     │
│  cooling      [━━━━●━━━] 120   │  gate     [━●━━━━━] 0.04   │
│  sparkchance  [━━●━━━━━] 0.35  │  gain     [━━━━●━━] 3.00   │
│  sparkheatmin [━━━●━━━━] 80    │  attack   [━●━━━━━] 0.08   │
│  sparkheatmax [━━━━━━●━] 220   │  release  [━━●━━━━] 0.30   │
├─────────────────────────────────────────────────────────────┤
│  CONSOLE                                                    │
│  > json info                                                │
│  {"device":"Tube Light","version":"0.3.0"...}               │
│  > set cooling 150                                          │
│  cooling = 150  [fire]                                      │
│  > [____________________________________] [Send]            │
└─────────────────────────────────────────────────────────────┘
```

---

## Success Criteria

MVP is done when you can:
1. Open the web app in Chrome/Edge
2. Click Connect and select the serial port
3. See device info populate automatically
4. See settings populate sliders dynamically (grouped by category)
5. Move a slider and see device respond
6. See audio waveform scroll in real-time
7. Type commands in console

---

## Deployment

```bash
# Build
npm run build

# Deploy to Firebase
firebase deploy --only hosting
```

**URL:** `https://<project-id>.web.app`

---

## Future Enhancements (Post-MVP)
- PWA offline support with service worker
- Settings presets (save/load configurations)
- Dark/light theme toggle
- LED matrix preview visualization
- Export audio recording to file
- Multiple device support

Ship it, then iterate.
