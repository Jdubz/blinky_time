# Blinky Things - Build Guide

## Supported Hardware

| Board | FQBN | Core | Makefile Target |
|-------|------|------|-----------------|
| XIAO nRF52840 Sense | `Seeeduino:nrf52:xiaonRF52840Sense` | Seeeduino nRF52 (non-mbed) | `compile`, `uf2-upload` |
| ~~XIAO ESP32-S3 Sense~~ | ~~`esp32:esp32:XIAO_ESP32S3`~~ | ~~arduino-esp32~~ | ~~`esp32-compile`, `esp32-uf2-upload`~~ |

> **Note:** ESP32-S3 support was cut in March 2026. All active development targets nRF52840 only.

**CRITICAL**: Never use `arduino-cli upload` for the nRF52840 — it will brick the device. Use UF2 upload only. See [SAFETY.md](SAFETY.md).

---

## Environment Setup

### Prerequisites
- Arduino CLI (tested with 1.3.1+)
- Python 3 with `pyserial` (`pip3 install pyserial`)

### 1. Install Arduino CLI
```bash
# Linux/macOS
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

# Windows: download from https://arduino.cc/pro/cli
# Or use the local copy at: D:\utilities\arduino-cli.exe
```

### 2. Install nRF52840 Core
```bash
arduino-cli config add board_manager.additional_urls \
  https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:nrf52
```

**Important**: Use `Seeeduino:nrf52` (non-mbed). The mbed core (`Seeeduino:mbed`) has header conflicts with Adafruit NeoPixel.

### 3. Install ESP32 Core
```bash
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### 4. Install Required Libraries
```bash
arduino-cli lib install "Adafruit NeoPixel"
arduino-cli lib install "Seeed Arduino LSM6DS3"
```

---

## Building

### nRF52840 (Hat / Tube Light / Bucket Totem)

```bash
# Compile only (safe)
make compile

# Compile to output directory for UF2 upload
make compile-out

# Full UF2 upload (Linux/headless)
make uf2-upload UPLOAD_PORT=/dev/ttyACM0

# Dry run (compile + validate, no upload)
make uf2-check UPLOAD_PORT=/dev/ttyACM0
```

Windows (compile only via Arduino IDE for upload):
```powershell
arduino-cli compile --fqbn "Seeeduino:nrf52:xiaonRF52840Sense" blinky-things
```

### ESP32-S3 (Display board — 32×32 LED matrix)

```bash
# Compile only
make esp32-compile

# Full UF2 upload (Linux/headless)
make esp32-uf2-upload UPLOAD_PORT=/dev/ttyACM0

# Dry run
make esp32-uf2-check UPLOAD_PORT=/dev/ttyACM0
```

Direct arduino-cli:
```bash
arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32S3" \
  --output-dir /tmp/blinky-esp32-build blinky-things/
```

---

## UF2 Upload Workflow

Both boards use the UF2 mass-storage bootloader. The upload script (`tools/uf2_upload.py`) handles the full flow automatically:

```
nRF52840:  compile → .hex → uf2conv.py (family 0xADA52840) → .uf2 → copy to drive
ESP32-S3:  compile → .bin → uf2conv.py (family 0xc47e5767, base 0x10000) → .uf2 → copy to drive
```

### Entering Bootloader Mode
Both boards: **double-tap the Reset button**. A USB mass-storage drive appears.

| Board | Drive Label | Normal VID:PID | Bootloader VID:PID |
|-------|-------------|----------------|---------------------|
| nRF52840 | `XIAO-SENSE` | `2886:8045` | `2886:0045` |
| ESP32-S3 | `ESP32S3` *(TO VERIFY)* | `303A:1001` ✓ verified | `303A:0002` *(TO VERIFY)* |

To verify ESP32-S3 VID/PIDs, run `lsusb` in both normal and bootloader modes, then update `BOARD_PROFILES` in `tools/uf2_upload.py`.

### Manual Upload (Windows, any board)
1. Double-tap Reset — drive appears (e.g., `XIAO-SENSE`)
2. Drag and drop the `.uf2` file onto the drive
3. Board reboots automatically

---

## Device Configurations

Device configs are JSON files uploaded via the serial console at runtime — not compile-time flags. The firmware is the same binary for all devices of the same platform.

```bash
# Upload a device config via MCP or serial console
device upload <json-string>
```

Config files live in `devices/registry/`:

| File | Device | LEDs | Layout |
|------|--------|------|--------|
| `hat.json` | Hat | 89 | LINEAR |
| `tube_light.json` | Tube Light | 60 | MATRIX 4×15 |
| `long_tube.json` | Long Tube | 120 | MATRIX |
| `bucket_totem.json` | Bucket Totem | 128 | MATRIX 16×8 |
| `display_v1.json` | Display (ESP32-S3) | 1024 | MATRIX 32×32 |

---

## Multi-Platform Architecture

The firmware supports both platforms from a single codebase via compile-time detection in `hal/PlatformDetect.h`:

```cpp
#if defined(ESP32)
  #define BLINKY_PLATFORM_ESP32S3 1
#elif defined(ARDUINO_ARCH_MBED) || defined(NRF52) || ...
  #define BLINKY_PLATFORM_NRF52840 1
#endif
```

Platform-specific code is isolated in HAL implementations:

| Interface | nRF52840 | ESP32-S3 |
|-----------|----------|----------|
| PDM mic | `Nrf52PdmMic` (hardware PDM interrupt) | `Esp32PdmMic` (I2S polling) |
| Flash storage | `FlashIAP` / `InternalFileSystem` | `Preferences` (NVS) |
| Battery monitor | GPIO+ADC circuit | Disabled (no battery circuit) |

See [DEVELOPMENT.md](DEVELOPMENT.md) for the full multi-platform guide.

---

## Build Output

### nRF52840 (typical)
```
Sketch uses ~172 KB (21%) of program storage. Maximum is 811 KB.
Global variables use ~11 KB (4%) of dynamic memory.
```

### ESP32-S3 (typical)
```
Sketch uses ~480 KB (5.7%) of program storage. Maximum is 8 MB.
Global variables use ~30 KB of dynamic memory.
```
_(Higher than nRF52 due to ESP-IDF system libraries and WiFi stack linked in by default.)_

---

## Troubleshooting

### "Header conflict / redefinition of struct _PinDescription"
You're using the mbed core. Switch to `Seeeduino:nrf52` (non-mbed).

### "uf2conv.py not found"
Install the Seeeduino nRF52 board package — it ships `uf2conv.py` which is also used for ESP32-S3 conversion.

### ESP32-S3 UF2 drive not detected
- Verify the drive label in `BOARD_PROFILES["esp32s3"]["drive_label"]` matches what appears in `lsblk`
- Verify `bootloader_pid` matches `lsusb` output after double-tap reset

### Port locked / device not found
The MCP server or blinky-console may have the port open. Disconnect all serial sessions before uploading.
