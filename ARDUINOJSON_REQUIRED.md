# ArduinoJson Library Required

## Installation

The runtime device configuration feature requires the **ArduinoJson** library for parsing uploaded JSON configurations.

You don't install it by hand. `scripts/setup_toolchain.sh` installs the version pinned in `tools/firmware_toolchain.json` into the repo's isolated toolchain (`.arduino/`), along with the core and the other libraries:

```bash
bash scripts/setup_toolchain.sh
```

`scripts/build.sh` compiles only against that toolchain, and `tools/check_build_libs.py` fails the build if ArduinoJson resolves from anywhere else or its version drifts from the pin. Don't install ArduinoJson through the Arduino IDE Library Manager or into `Documents/Arduino/libraries` to "fix" a build; fix the pin and re-run setup (see [docs/RELEASE_PROCESS.md](docs/RELEASE_PROCESS.md#the-pinned-toolchain-every-machine)).

## Version Requirements

- **Pinned version**: 7.4.2 (`tools/firmware_toolchain.json`)
- The code uses the ArduinoJson 7 API (`JsonDocument`). Versions 6.x and 5.x are not compatible.

## Usage in Blinky

ArduinoJson is used for (`blinky-things/inputs/SerialConsole.cpp`):
- Parsing device configuration JSON via the `device upload <json>` command
- Serializing device configuration for the `device show` command

## Size Impact

- **Program Size**: +10-15 KB
- **RAM Usage**: ~1-2 KB per parsing operation
- **Flash Storage**: No impact (JSON is parsed, not stored)

---

**Status**: Required since firmware v28 (runtime device configuration)
**Documentation**: https://arduinojson.org/
