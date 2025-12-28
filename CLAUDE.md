# Claude Code Instructions for Blinky Project

## CRITICAL: NEVER FLASH FIRMWARE VIA CLI

**DO NOT use arduino-cli, Bash, or any command-line tool to upload/flash firmware!**

- `arduino-cli upload` WILL BRICK THE DEVICE
- The device CANNOT be recovered without SWD hardware (J-Link, etc.)
- This is due to a Seeeduino mbed platform bug

### Safe Operations

**ALLOWED via CLI:**
- `arduino-cli compile` - Compiling is safe
- `arduino-cli core list/install` - Core management is safe
- Reading serial ports is safe

**NEVER DO via CLI:**
- `arduino-cli upload` - WILL CORRUPT BOOTLOADER
- Any command that writes to the device

### If the Device Becomes Unresponsive

1. Double-tap the reset button quickly (like double-click)
2. A drive letter should appear (e.g., "XIAO-SENSE")
3. The user can then flash via Arduino IDE

### Why This Happens

The Seeeduino mbed platform's arduino-cli upload routine starts writing
firmware before properly verifying the bootloader state, causing partial
writes that corrupt the bootloader region.

## Compilation Commands

Use the arduino-cli for compilation only:
```bash
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense blinky-things
```

**User must flash via Arduino IDE after compilation.**
