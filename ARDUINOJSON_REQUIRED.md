# ArduinoJson Library Required

## Installation

The runtime device configuration feature requires the **ArduinoJson** library for parsing uploaded JSON configurations.

### Via Arduino IDE

1. Open Arduino IDE
2. Go to **Sketch** → **Include Library** → **Manage Libraries...**
3. Search for "ArduinoJson"
4. Install **ArduinoJson** by Benoit Blanchon (version 6.x or later)
5. Close the Library Manager

### Via Arduino CLI

```bash
arduino-cli lib install ArduinoJson
```

### Manual Installation

1. Download from: https://github.com/bblanchon/ArduinoJson/releases
2. Extract to your Arduino libraries folder:
   - Windows: `Documents\Arduino\libraries\`
   - Mac: `~/Documents/Arduino/libraries/`
   - Linux: `~/Arduino/libraries/`

## Version Requirements

- **Minimum Version**: 6.18.0
- **Recommended Version**: Latest 6.x release
- **NOT Compatible**: Version 5.x (different API)

## Usage in Blinky

ArduinoJson is used for:
- Parsing device configuration JSON via `upload config` command
- Serializing device configuration for `show config` command
- Web console JSON communication

## Size Impact

- **Program Size**: +10-15 KB (dynamic allocation variant)
- **RAM Usage**: ~1-2 KB per parsing operation
- **Flash Storage**: No impact (JSON is parsed, not stored)

The library is well-optimized for embedded systems and is widely used in Arduino projects.

## Alternative (Future)

For minimal flash usage, a custom lightweight JSON parser could be implemented, but ArduinoJson provides:
- Robust error handling
- Standards-compliant parsing
- Zero-copy operation when possible
- Extensive validation
- Active maintenance

---

**Status**: Required for firmware v28+ (runtime device configuration)
**Documentation**: https://arduinojson.org/
