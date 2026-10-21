# Device Registry

This directory contains JSON configuration files for different Blinky hardware devices. Each JSON file defines a complete device configuration that is uploaded to the firmware with the `device upload <json>` console command.

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
| `hat_v1` | Festival Hat v1 | 89 (linear string) | LINEAR, single strand on D10. Battery-powered. The release test device (`devices/test_devices.json`). | `hat_v1.json` |
| `sourpuss_hat` | Sourpuss Hat | 70 (linear string) | LINEAR, single strand on D10. Battery-powered. New hat, distinct from `hat_v1`. | `sourpuss_hat.json` |
| `scarf` | Scarf | 141 (linear string) | LINEAR, single strand on D0. Battery-powered. | `scarf.json` |
| `shannon_hat` | Shannon Hat | 79 (linear string) | LINEAR, single strand on D0. Battery-powered. Same build as `scarf` apart from LED count. | `shannon_hat.json` |
| `tube_v2` | Tube Light v2 | 60 (4×15 matrix) | MATRIX, vertical zigzag on D10. Battery-powered. | `tube_v2.json` |
| `bucket_v3` | Bucket Totem v3 | 128 (16×8 matrix) | MATRIX, horizontal row-major on D10. Z-inverted. | `bucket_v3.json` |
| `big_bucket_v1` | Big Bucket | 112 (14×8 matrix) | MATRIX, horizontal zigzag (serpentine) on D0. A generator-cycle button is wired on D1 but disabled (`buttonPin=0`) because of ghost presses (#222). **Outlier:** `ledType=6` (NEO_RGB) because the LED part is wired native RGB, not the fleet-standard GRB. | `big_bucket_v1.json` |
| `cart_inner` | Cart Inner | 104 (linear, two 52-LED strands) | LINEAR, strand 1 on D10 + strand 2 on D9; firmware splits the 104-pixel buffer in half across the two pins. | `cart_inner.json` |
| `cart_outer` | Cart Outer | 96 (linear single strand) | LINEAR, single strand on D10. | `cart_outer.json` |
| `long_tube_v1` | Long Tube | 240 (4×60 matrix) | MATRIX, vertical zigzag on D10. | `long_tube_v1.json` |
| `cart_umbrella_v1` | Cart Umbrella | 104 (8×13 matrix) | MATRIX, `VERTICAL_FIRST_DOWN` (orientation 4) on D10. Confirmed against hardware 2026-05-18. | `cart_umbrella_v1.json` |
| `display_v1` | Display | 1024 (32×32 panel grid) | MATRIX, PANEL_GRID orientation on D10. **ESP32-S3 platform retired 2026-03** — hardware exists but no firmware path today (next gen: `docs/NEXTGEN_ESP32S3_PLAN.md`). | `display_v1.json` |

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
  "ledPin":     number,    // GPIO pin for LED data (0-47 on nRF52840; default 10 if omitted)
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
| `battery` | `false` | Battery-equipped devices only (today: tube_v2, hat_v1, sourpuss_hat, scarf, shannon_hat). When `true`, the firmware enables battery monitoring + fast charge and applies the **static** `Platform::Battery::*` thresholds (LiPo: low=3.5V, critical=3.3V, min=3.0V, max=4.2V). These values are NOT configurable per device — they are chemistry-derived constants in `blinky-things/hal/PlatformConstants.h`. When `false`/omitted, battery code paths are skipped entirely. |
| `upVectorX` / `upVectorY` / `upVectorZ` | `0` / `0` / `1` | When the device is mounted with a non-vertical "up" |
| `rotationDegrees` | `0` | When the device's "north" is rotated relative to the chip |
| `invertZ` | `false` | LEDs facing downward (bucket_v3) |
| `swapXY` | `false` | LED data path runs columns-as-rows |
| `invertX` / `invertY` | `false` | Strip wired right-to-left or bottom-to-top |
| `baudRate` | `115200` | Never — universal |
| `initTimeoutMs` | `2000` | Never — universal |
| `sampleRate` | `16000` | Never — universal (other valid: 8000/32000/44100/48000) |
| `bufferSize` | `32` | Never — universal |
| `buttonPin` | `0` (no button) | When a physical generator-cycle button is wired and wanted (big_bucket_v1 has one on D1, currently disabled). Must differ from `ledPin` / `ledPin2` |

### ledType constants

The lower 8 bits encode the R/G/B byte ordering. The driver only
allocates 3 bytes per pixel, so NEO_RGBW-style values (any offset > 2)
are rejected at construction with an `[ERROR]` log.

- `82` = `NEO_GRB + NEO_KHZ800` — standard WS2812B (fleet default for new configs)
- `6` = `NEO_RGB` — used by big_bucket_v1 because that panel's LED part is wired in native RGB order

**Do NOT use `12390` (0x3066).** Earlier revisions of this doc claimed `12390 = NEO_GRB + NEO_KHZ800`. That's incorrect — `NEO_GRB + NEO_KHZ800` is `0x52 = 82`. The lower byte of 12390 is `0x66 = 0b01100110`, which the firmware decodes as `rOffset=2, gOffset=1, bOffset=2`. With R and B both at byte-offset 2, every pixel write puts R and B at the same byte slot (visible as blue-channel-missing colors on pre-`752ebc16` builds). Post-`752ebc16` the driver validates and refuses to construct, the `if (!asyncStrip->isValid())` check in setup hits `haltWithError`, the WDT trips, and after 3 boot failures the device falls into BLE-DFU recovery — exactly the configured-boot crash observed 2026-05-18 on cart_inner and cart_outer.

### Orientation values

- `0` = HORIZONTAL — plain row-major (data flows L→R every row; use when each row is a separate strip jumpered back to column 0)
- `1` = VERTICAL — column-major zigzag (data snakes column-by-column, common for vertical tube fixtures)
- `2` = PANEL_GRID — 2×2 of equal sub-panels chained TL→TR→BL→BR, each sub-panel row serpentine
- `3` = HORIZONTAL_ZIGZAG — row-major serpentine: row 0 L→R, row 1 R→L, row 2 L→R, … (single strip snaked back and forth, big-bucket-style hand-wired panels)
- `4` = VERTICAL_FIRST_DOWN — column-major, NO zigzag. Column 0 data enters at the TOP and flows down. Columns 1..N each have data entering at the BOTTOM, flowing up — ALL in the same direction (no alternation between adjacent columns). Used on `cart_umbrella_v1`: the first spoke's data comes off the controller down the canopy-to-tip leg; from the tip a long jumper runs to each subsequent spoke's tip, so every other spoke is wired bottom→top.

### Battery support

A single boolean field — `"battery": true` — switches on monitoring +
fast charge for battery-equipped devices (tube_v2, hat_v1, sourpuss_hat,
scarf, shannon_hat today). The
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

`device upload` writes the config to flash, so it is a device-mutating command.

- **Fleet devices (through blinky-server):** `device upload` and `reboot` on `/api/devices/{id}/command` are gated on the `X-Deploy-Tool` header, which only `scripts/deploy.sh` sets. A direct `curl` returns 403, and that's intended. Don't script config changes over the API. Never loop config flips across fleet devices. See CLAUDE.md ("Only stable releases reach fleet and new devices" and the 60-second rule) and `docs/SAFETY.md`.
- **A bench device on this machine's USB:** send `device upload <json>` over its serial port (e.g. the blinky-console serial terminal), where `<json>` is the file's contents on one line (`jq -c . devices/registry/<id>.json`). Never do this on an enrolled fleet device.

After an upload, wait at least 75 s of uptime before any reset, so `markStable()` fires (CLAUDE.md's 60-second rule). Then one verification reboot is fine; wait another 75 s after it.

## Creating a Custom Device Config

1. Copy an existing JSON file (e.g., `hat_v1.json`)
2. Rename it with a unique device ID (e.g., `custom_hat_v2.json`)
3. Modify the values:
   - Update `deviceId` and `deviceName`
   - Set `ledWidth` and `ledHeight` to match your LED count
   - Set `ledPin` to your data pin number
   - Adjust `brightness` for your preference
   - Set `"battery": true` if the device has a battery
4. Upload over serial to the device on your USB (see above)

## Validation

The firmware validates every stored configuration (`DeviceConfigLoader::validate`):
- LED count must be 1-2048
- `ledPin`, `ledPin2` and `buttonPin` must be 0-47 on nRF52840, and must all differ; a two-strand config needs an even LED count
- `ledType`'s R/G/B byte offsets must be in range and distinct
- `orientation` 0-4, `layoutType` 0-2, `brightness` ≤ 255

Invalid configurations are rejected with a log message. A non-standard sample rate or baud rate only logs a warning.

## Troubleshooting

**Device won't boot after config upload:**
- Check serial console for validation errors
- Ensure LED count matches physical hardware
- Verify LED pin is correct
- Re-upload a known-good config

**LEDs show wrong colors:**
- Check `ledType` value (may need NEO_RGB instead of NEO_GRB)
- Verify wiring matches configuration

**Safe mode after reboot:**
- Config may have failed validation
- Or the reboot-counter quarantine fired after too many fast resets (CLAUDE.md, 60-second rule)
- Check serial console for error messages
- Re-upload that device's own registry config, then wait 75 s before any reset

## Notes

- The firmware stores ONE device config in flash (~160 bytes)
- Config persists across reboots and power cycles
- Uploading a new config overwrites the previous one
- First boot with no config enters safe mode (LED output disabled)
- All devices share the same universal firmware binary

---

**Last Updated**: October 2026
