# Hardware Documentation

Hardware reference for Blinky Time controllers. The canonical, authoritative list of supported devices is in `devices/registry/*.json` — this doc summarises the recurring patterns.

## Device Configurations (nRF52840 XIAO Sense)

Current registered devices (see `devices/registry/`):

| Device | Layout | Notes |
|--------|--------|-------|
| `hat_v1` | 89×1 string | Wearable, battery. The release test device |
| `sourpuss_hat` | 70×1 string | Wearable, battery |
| `shannon_hat` | 79×1 string | Wearable, battery, `ledPin=0` |
| `scarf` | 141×1 string | Wearable, battery, `ledPin=0` |
| `tube_v2` | 4×15 zigzag matrix | Standalone tube |
| `long_tube_v1` | 4×60 zigzag matrix | Sealed sculpture |
| `bucket_v3` | 16×8 matrix | Standalone bucket |
| `big_bucket_v1` | 14×8 matrix | `ledPin=0`, `ledType=6` (RGB), sealed |
| `cart_inner` | 104×1 string | Lemon-cart, two channels (`ledPin=10`, `ledPin2=9`) |
| `cart_outer` | 96×1 string | Lemon-cart |
| `cart_umbrella_v1` | 8×13 matrix | Lemon-cart umbrella |
| `display_v1` | 32×32 matrix | Bench display; ESP32-S3 hardware with no firmware path today |

Default LED data pin is **D10**. `big_bucket_v1`, `scarf` and `shannon_hat` use D0. `cart_inner` adds a second channel on D9 for its split topology.

## Bootloader and flashing

Every fleet device runs bootloader **0.8.0-10** (UF2 drive over USB plus BLE DFU). Firmware goes on only through `./scripts/deploy.sh` (fleet), `tools/deploy_local.py` (a device on your USB) or the browser updater (`docs/WEB_FLASHER.md`). Bootloader changes go only through `./scripts/deploy-bootloader.sh`, on a sacrificial device first. Never use `arduino-cli upload` or `adafruit-nrfutil dfu serial`. See CLAUDE.md and `docs/SAFETY.md`.

The nRF52840 Sense also has 2 MB of external QSPI flash. Signed OTA updates stage the new image there while the old firmware keeps running (`blinky-things/ota/QspiOtaStaging.h`).

## Wiring Diagrams

### Basic Connections
```
nRF52840 XIAO Sense Pinout:
├── D10 (GPIO) → LED Data Pin
├── 3V3 → LED Power (VCC)
├── GND → LED Ground
├── Built-in Mic → Audio input
└── Built-in IMU → Motion sensing
```

### Power Requirements
- **LED Current**: ~60mA per LED at full brightness
- **Controller Current**: ~50mA during operation
- **Battery Recommendation**: 3.7V Li-Po, 1000mAh+ for portable use

## LED Strip Specifications

### Supported Types
- **WS2812B** (NeoPixel) - Most common, NEO_GRB color order
- **SK6812** - Similar to WS2812B with white channel option
- **WS2813** - Backup data line for improved reliability

### Color Order Configuration
Set by `ledType` in the device config (a number; see `devices/registry/README.md`):
- `82` = `NEO_GRB + NEO_KHZ800`: standard WS2812B, the fleet default
- `6` = `NEO_RGB`: `big_bucket_v1`, whose LEDs are wired native RGB

RGBW types are rejected: the driver uses 3 bytes per pixel.

## Installation Notes

### Zigzag Matrix Wiring
For tube lights and matrix displays, LEDs are wired in a zigzag pattern:
```
Column 0: LEDs 0-14 (bottom to top)
Column 1: LEDs 29-15 (top to bottom) 
Column 2: LEDs 30-44 (bottom to top)
Column 3: LEDs 59-45 (top to bottom)
```

### Enclosure Considerations
- **Waterproofing**: IP65 rating recommended for outdoor use
- **Heat Dissipation**: Provide ventilation for high LED counts
- **Access Ports**: Programming connector and charging port access

## Troubleshooting

### Common Issues
1. **Wrong Color Order** - Check `ledType` (82 = GRB, 6 = RGB) in the device config
2. **Power Supply** - Insufficient current causes dim/flickering LEDs
3. **Data Signal** - Use proper gauge wire for data connections
4. **Ground Loops** - Ensure common ground between controller and LEDs

### Testing Procedures
1. **Single LED Test** - Verify basic connectivity
2. **Color Order Test** - Check red/green/blue display correctly  
3. **Full Matrix Test** - Validate zigzag mapping pattern
4. **Audio Test** - Confirm microphone responsiveness