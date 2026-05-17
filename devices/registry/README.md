# Device Registry

This directory contains JSON configuration files for different Blinky hardware devices. Each JSON file defines a complete device configuration that can be uploaded to the firmware via serial or web console.

**Source of truth:** these files are the intended-canonical config for each
physical device. To push a registry entry to a connected device today, send
its full contents as a single `device upload <json>` command via the
`X-Deploy-Tool`-gated fleet-API endpoint. There is no `deploy-config.sh`
wrapper yet — that's a planned addition so the registry can be enforced
in the same way `deploy.sh` enforces firmware. Until that lands, treat
`git diff` of this directory against the live device's `device show`
output as the audit path; any drift wants fixing on one side or the
other.

## Available Devices

| Device ID | Description | LEDs | Layout | File |
|-----------|-------------|------|--------|------|
| `hat_v1` | Festival Hat v1 | 89 (linear string) | LINEAR, single strand on D0. Battery-powered. | `hat_v1.json` |
| `tube_v2` | Tube Light v2 | 60 (4×15 matrix) | MATRIX, vertical zigzag on D10. Battery-powered. | `tube_v2.json` |
| `bucket_v3` | Bucket Totem v3 | 128 (16×8 matrix) | MATRIX, horizontal row-major on D10. Z-inverted. | `bucket_v3.json` |
| `big_bucket_v1` | Big Bucket | 112 (14×8 matrix) | MATRIX, horizontal zigzag (serpentine) on D0; button on D1 cycles generator. **Outlier:** `ledType=6` (NEO_RGB) because the LED part is wired native RGB, not the fleet-standard GRB. | `big_bucket_v1.json` |
| `cart_inner` | Cart Inner | 104 (linear, two 52-LED strands) | LINEAR, strand 1 on D10 + strand 2 on D9; firmware splits the 104-pixel buffer in half across the two pins. | `cart_inner.json` |
| `cart_outer` | Cart Outer | 96 (linear single strand) | LINEAR, single strand on D10. | `cart_outer.json` |
| `long_tube_v1` | Long Tube | 240 (4×60 matrix) | MATRIX, vertical zigzag on D10. | `long_tube_v1.json` |
| `cart_umbrella_v1` | Cart Umbrella | 128 (8×16 matrix) | MATRIX, horizontal row-major on D10. Z-inverted (mounted pointing down). **In progress** — specs unverified against hardware. | `cart_umbrella_v1.json` |
| `display_v1` | Display | 1024 (32×32 panel grid) | MATRIX, PANEL_GRID orientation on D10. **ESP32-S3 platform retired 2026-03** — hardware exists but no firmware path today. | `display_v1.json` |

## JSON Schema

**Design principle: only specify what's actually unique to this device.**
Every field has a firmware default in `SerialConsole::uploadDeviceConfig`
that gets applied when the field is missing from the JSON. The registry
files are intentionally minimal — fields whose value equals the
firmware default should be omitted, so a glance at the JSON shows
only the per-device facts.

### Required fields

These must be present:

```json
{
  "deviceId":   "string",  // unique ID (alphanumeric + underscore)
  "deviceName": "string",  // human-readable name (max 31 chars)
  "ledWidth":   number,    // matrix width, or total count for linear strips
  "ledHeight":  number,    // matrix height; 1 for linear strips
  "ledPin":     number,    // GPIO pin for LED data (0-47 on nRF52840)
  "ledType":    number,    // see ledType constants below
  "orientation": number,   // see orientation values below
  "layoutType": number     // 0=MATRIX, 1=LINEAR, 2=RANDOM
}
```

### Optional fields (with firmware defaults)

Add only when this device needs a non-default value:

| Field | Default | When to override |
|---|---|---|
| `ledPin2` | `0` (single-strand) | Multi-strand devices (e.g. cart_inner has two 52-LED strands on pins 10 + 9; firmware splits the buffer in half) |
| `brightness` | `100` | Per-device tuning |
| `battery` | `false` | Battery-equipped devices only (today: tube_v2, hat_v1). When `true`, the firmware enables battery monitoring + fast charge and applies the **static** `Platform::Battery::*` thresholds (LiPo: low=3.5V, critical=3.3V, min=3.0V, max=4.2V). These values are NOT configurable per device — they are chemistry-derived constants in `blinky-things/hal/PlatformConstants.h`. When `false`/omitted, battery code paths are skipped entirely. |
| `upVectorX` / `upVectorY` / `upVectorZ` | `0` / `0` / `1` | When the device is mounted with a non-vertical "up" |
| `rotationDegrees` | `0` | When the device's "north" is rotated relative to the chip |
| `invertZ` | `false` | LEDs facing downward (bucket_v3, cart_umbrella_v1) |
| `swapXY` | `false` | LED data path runs columns-as-rows |
| `invertX` / `invertY` | `false` | Strip wired right-to-left or bottom-to-top |
| `baudRate` | `115200` | Never — universal |
| `initTimeoutMs` | `2000` | Never — universal |
| `sampleRate` | `16000` | Never — universal (other valid: 8000/32000/44100/48000) |
| `bufferSize` | `32` | Never — universal |
| `buttonPin` | `0` (no button) | When a physical generator-cycle button is wired (big_bucket_v1 on D1). Must differ from `ledPin` / `ledPin2` |

### ledType constants

The lower 8 bits encode the R/G/B byte ordering. The driver only
allocates 3 bytes per pixel, so NEO_RGBW-style values (any offset > 2)
are rejected at construction with an `[ERROR]` log.

- `12390` = `NEO_GRB + NEO_KHZ800` — standard WS2812B (fleet default for new configs)
- `82` = same NEO_GRB byte ordering, no speed-flag bits — functionally identical to 12390 on this firmware (the driver masks to lower 8 bits)
- `6` = `NEO_RGB` — used by big_bucket_v1 because that panel's LED part is wired in native RGB order

### Orientation values

- `0` = HORIZONTAL — plain row-major (data flows L→R every row; use when each row is a separate strip jumpered back to column 0)
- `1` = VERTICAL — column-major zigzag (data snakes column-by-column, common for vertical tube fixtures)
- `2` = PANEL_GRID — 2×2 of equal sub-panels chained TL→TR→BL→BR, each sub-panel row serpentine
- `3` = HORIZONTAL_ZIGZAG — row-major serpentine: row 0 L→R, row 1 R→L, row 2 L→R, … (single strip snaked back and forth, big-bucket-style hand-wired panels)

### Battery support

A single boolean field — `"battery": true` — switches on monitoring +
fast charge for battery-equipped devices (tube_v2, hat_v1 today). The
threshold values are NOT configurable per device; they're chemistry-
constants in `blinky-things/hal/PlatformConstants.h`:

- **VOLTAGE_FULL**: 4.2 V (fully charged, LiPo)
- **VOLTAGE_LOW** = `DEFAULT_LOW_THRESHOLD`: 3.5 V (~10%, alert)
- **VOLTAGE_CRITICAL** = `DEFAULT_CRITICAL_THRESHOLD`: 3.3 V (~0%, shutdown soon)
- **VOLTAGE_EMPTY**: 3.0 V (over-discharge protection)

When `battery: false` (or omitted) the firmware skips `battery->begin()`
entirely — no setFastCharge call, no periodic ADC reads, no threshold
comparisons. Non-battery hardware leaves the BQ24074 unpowered, so
those operations were always no-ops; the explicit gate just makes the
intent unambiguous.

## Uploading a Device Config

**All device management goes through blinky-server** (`http://blinkyhost.local:8420`).
Do not interact with serial ports directly.

### Via blinky-server API (recommended)

```bash
# 1. Build the command payload with jq (handles JSON escaping correctly)
jq -n --arg cmd "device upload $(jq -c . devices/registry/hat_v1.json)" \
  '{command: $cmd}' | \
  curl -X POST http://blinkyhost.local:8420/api/devices/{device_id}/command \
    -H 'Content-Type: application/json' -d @-

# 2. Reboot to apply
curl -X POST http://blinkyhost.local:8420/api/devices/{device_id}/command \
  -H 'Content-Type: application/json' \
  -d '{"command": "reboot"}'

# 3. Verify after reconnect (~10s)
curl http://blinkyhost.local:8420/api/devices/{device_id}
```

### Via Web Console
1. Open the web console at http://localhost:3000 (or device IP)
2. Navigate to Settings → Device Configuration
3. Select a device from the dropdown OR paste custom JSON
4. Click "Upload Config"
5. Reboot the device

## Creating a Custom Device Config

1. Copy an existing JSON file (e.g., `hat_v1.json`)
2. Rename it with a unique device ID (e.g., `custom_hat_v2.json`)
3. Modify the values:
   - Update `deviceId` and `deviceName`
   - Set `ledWidth` and `ledHeight` to match your LED count
   - Set `ledPin` to your data pin number
   - Adjust `brightness` for your preference
   - Configure battery thresholds if needed
4. Upload via serial or web console

## Validation

The firmware validates all uploaded configurations:
- LED count must be 1-500
- LED pin must be 0-48 (nRF52840 GPIO range)
- Voltage range must be 2.5V - 5.0V
- Sample rate must be standard (8000, 16000, 32000, 44100, 48000 Hz)
- Baud rate must be standard (9600, 19200, 38400, 57600, 115200, 230400)

Invalid configurations will be rejected with an error message.

## Troubleshooting

**Device won't boot after config upload:**
- Check serial console for validation errors
- Ensure LED count matches physical hardware
- Verify LED pin is correct
- Try factory reset and re-upload

**LEDs show wrong colors:**
- Check `ledType` value (may need NEO_RGB instead of NEO_GRB)
- Verify wiring matches configuration

**Safe mode after reboot:**
- Config may have failed validation
- Check serial console for error messages
- Upload a known-good config (e.g., `hat_v1.json`)

## Notes

- The firmware stores ONE device config in flash (~160 bytes)
- Config persists across reboots and power cycles
- Uploading a new config overwrites the previous one
- First boot with no config enters safe mode (LED output disabled)
- All devices share the same universal firmware binary

---

**Last Updated**: January 2026
**Firmware Version**: v28+ (runtime device configuration)
