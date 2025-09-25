# Blinky Time Build System

Cross-platform automated build system for the Blinky Time LED project using Make.

## Quick Start

### All Platforms (Make)
```bash
# Upload device type 2 (Tube Light) to default port
make upload DEVICE=2

# Upload to specific port
make upload DEVICE=1 PORT=COM3

# Compile only
make compile DEVICE=3

# Monitor serial output
make monitor PORT=COM3

# List connected boards
make list-boards

# Get help
make help
```

## Installation

### Windows
1. **Install Make (Optional but Recommended):**
   ```powershell
   winget install GnuWin32.Make
   # OR
   choco install make
   # OR 
   scoop install make
   ```

2. **Arduino CLI is required** (if not already installed):
   ```powershell
   .\make.ps1 install-deps
   ```

### Linux/macOS
1. **Make is usually pre-installed**. If not:
   ```bash
   # Ubuntu/Debian
   sudo apt install build-essential
   
   # macOS
   xcode-select --install
   ```

2. **Install dependencies:**
   ```bash
   make install-deps
   ```

## Device Types

| Device | ID | Description | LED Count |
|--------|----| ----------- |-----------|
| Hat | 1 | Wearable hat with string configuration | 89 LEDs |
| Tube Light | 2 | 4x15 zigzag matrix tube | 60 LEDs |
| Bucket Totem | 3 | 16x8 matrix totem | 128 LEDs |

## Available Commands

### Core Commands
- `upload` - Compile and upload sketch to Arduino
- `compile` - Compile sketch only (no upload)
- `monitor` - Open serial monitor for debugging
- `clean` - Clean build artifacts

### Utility Commands  
- `list-boards` - Show connected Arduino boards
- `install-deps` - Install Arduino CLI and board packages
- `test` - Run compilation tests for all device types
- `help` - Show detailed help

### Development Commands
- `dev-setup` - Full development environment setup

## Parameters

### DEVICE / -Device
Device type to build for:
- `1` = Hat (89 LEDs)
- `2` = Tube Light (60 LEDs) - **Default**
- `3` = Bucket Totem (128 LEDs)

### PORT / -Port  
Serial port for upload/monitor:
- `auto` = Auto-detect connected Arduino - **Default**
- `COM3` = Specific Windows port
- `/dev/ttyUSB0` = Specific Linux port

## Examples

### Complete Workflow
```bash
# 1. Setup development environment (first time only)
make dev-setup

# 2. List connected boards
make list-boards

# 3. Compile and upload
make upload DEVICE=2

# 4. Monitor serial output
make monitor

# 5. Test all device types
make test
```

### Windows PowerShell Workflow
```powershell
# Quick upload with auto-detection
.\make.ps1 upload -Device 2

# Upload with specific port
.\make.ps1 upload -Device 2 -Port COM3

# Just compile to check for errors
.\make.ps1 compile -Device 2

# Monitor output
.\make.ps1 monitor -Port COM3
```

## Troubleshooting

### Upload Issues
1. **Check USB connection** - Ensure Arduino is properly connected
2. **Close Arduino IDE** - IDE can interfere with CLI uploads
3. **Try different USB port** - Some ports may have power issues
4. **Manual bootloader** - Double-tap reset button before upload
5. **Check port** - Run `make list-boards` to verify detection

### Compilation Issues
1. **Update dependencies** - Run `make install-deps`
2. **Clean build** - Run `make clean` then retry
3. **Check device type** - Ensure valid DEVICE parameter (1, 2, or 3)

### Platform Issues
- **Windows without Make**: Use `.\make.ps1` PowerShell wrapper
- **Permission denied**: Run as administrator on Windows
- **Command not found**: Ensure Arduino CLI is in PATH

## Hardware Support

Currently supports:
- **Board**: Seeed XIAO nRF52840 Sense
- **LEDs**: WS2812B addressable LED strips
- **Communication**: USB Serial (115200 baud)

## File Structure

```
blinky_time/
├── Makefile              # Cross-platform Make build system
├── make.ps1              # PowerShell wrapper for Windows
├── blinky-things/        # Arduino sketch directory
│   ├── blinky-things.ino # Main sketch file
│   └── ...               # Support files
├── scripts/              # Legacy build scripts  
└── BUILD_SYSTEM.md       # This documentation
```

## Integration

This build system integrates with:
- **VS Code**: Use tasks.json to run make commands
- **CI/CD**: GitHub Actions can use make commands
- **IDEs**: Any editor can invoke make/PowerShell commands

## Contributing

When adding new features:
1. Update both `Makefile` and `make.ps1`
2. Test on both Windows and Linux
3. Update this documentation
4. Add examples for new functionality

## License

Creative Commons Attribution-ShareAlike 4.0 International