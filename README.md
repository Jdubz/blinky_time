# 🔥 Blinky Time - tooling

[![License: CC BY-SA 4.0](https://img.shields.io/badge/License-CC%20BY--SA%204.0-lightgrey.svg)](http://creativecommons.org/licenses/by-sa/4.0/)
[![Platform](https://img.shields.io/badge/Platform-nRF52840-green.svg)](https://www.nordicsemi.com/Products/nRF52840)

Audio-reactive LED fixtures for wearable art and installations (hats, tubes, bucket totems, cart strings, matrices), all on the **Seeed XIAO nRF52840 Sense**. This repo is the tooling around them: the fleet server, the web console and updater, ML training, and the test box.

## Repositories (#357)

| Repo | What's in it | Here as |
|------|--------------|---------|
| **blinky_time** (this one) | blinky-server, blinky-console, ml-training, blinky-serial-mcp, test box scripts, fleet deploys, device configs | |
| **[blinky-firmware](https://github.com/Jdubz/blinky-firmware)** | the firmware (`blinky-things/`), bootloader, simulator, host tests, and the staging → hardware test → release flow | `firmware/` submodule |
| **[blinky-corpus](https://github.com/Jdubz/blinky-corpus)** | the validation corpus (`edm/`) and its scorers (`blinky_scoring`) | `corpus/` submodule |

Clone with `git clone --recurse-submodules https://github.com/Jdubz/blinky_time.git`, or run `git submodule update --init firmware corpus` in a clone. The firmware's own bootloader submodule isn't needed here.

The submodules are pinned: blinky-firmware follows its `master` (hardware-tested firmware), blinky-corpus its `main`. Dependabot opens a PR to bump each pin, and CI runs the server tests and the Python ↔ firmware parity check against it. The server, `deploy.sh` and the console's release mirror run the firmware's `tools/` at the pinned commit.

## What's here

```
blinky_time/
├── blinky-server/        # Fleet server: serial/BLE connections, the single flash path, deploys, validation, the test box's staging suite
├── blinky-console/       # Web console + browser firmware updater (React PWA, Firebase)
├── blinky-serial-mcp/    # MCP server for AI-assisted testing and tuning
├── blinky-test-player/   # Test audio player and tuning history
├── ml-training/          # Onset model training (runs on the DGX Spark as gpu_task jobs)
├── devices/registry/     # Per-device configs (layout, LED pin, LED count)
├── scripts/              # deploy.sh (fleet deploys), testbox/ (deployer, staging runner)
├── tools/                # Fleet and analysis tools (BLE DFU push, device eval, visual metrics)
├── docs/                 # Tooling docs (see docs/README.md)
├── firmware/             # blinky-firmware (submodule)
└── corpus/               # blinky-corpus (submodule)
```

## Updating devices

> ⚠️ **Never use `arduino-cli upload`, the Arduino IDE Upload button, or `adafruit-nrfutil dfu serial` on the nRF52840.** They can brick the device. See [CLAUDE.md](CLAUDE.md) and [docs/SAFETY.md](docs/SAFETY.md).

- **A device you own, no repo:** open [blinky-time.joshwentworth.com/#firmware](https://blinky-time.joshwentworth.com/#firmware) in Chrome or Edge and connect it over USB. It installs the latest **stable** release through the device's signed update path ([docs/WEB_FLASHER.md](docs/WEB_FLASHER.md)).
- **Fleet devices:** `./scripts/deploy.sh --devices=<id>` (latest stable release, with version check). `--devices=list` shows the candidates.
- **Building, test devices and releases** are blinky-firmware's: see its [README](https://github.com/Jdubz/blinky-firmware#readme) and [RELEASE_PROCESS.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/RELEASE_PROCESS.md).

A new chip boots unconfigured until it receives its config from `devices/registry/<id>.json` through `device upload <json>` (on a fleet device, through `deploy.sh` only). See [devices/registry/README.md](devices/registry/README.md).

## Development

**blinky-server:**
```bash
cd blinky-server
python -m venv venv && venv/bin/pip install -e '.[dev,ble]'
venv/bin/python -m pytest tests/
```

**blinky-console:**
```bash
cd blinky-console
npm install          # dependencies and git hooks (husky)
npm run dev          # dev server with hot reload
npm run test         # unit tests
npm run lint
npm run build
```

Changes go to `master` by pull request, with CI (`.github/workflows/pr-validation.yml`) green and the review addressed. See [CONTRIBUTING.md](CONTRIBUTING.md) and [.github/BRANCHING_STRATEGY.md](.github/BRANCHING_STRATEGY.md).

## Documentation

- [docs/README.md](docs/README.md): the tooling docs index
- [CLAUDE.md](CLAUDE.md): safety and workflow rules (flashing, releases, the test box). Read it first.
- [docs/TEST_BOX_BUILD_PLAN.md](docs/TEST_BOX_BUILD_PLAN.md): the test box and its deployer
- [blinky-firmware docs/WEB_FLASHER.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/WEB_FLASHER.md): browser firmware updates
- [docs/ML_IMPROVEMENT_PLAN.md](docs/ML_IMPROVEMENT_PLAN.md): ML roadmap
- Firmware architecture, hardware and audio docs: [blinky-firmware/docs](https://github.com/Jdubz/blinky-firmware/tree/staging/docs)

## License

[Creative Commons Attribution-ShareAlike 4.0 International](LICENSE).

## Credits

- **Hardware Platform**: [Seeed Studio XIAO nRF52840 Sense](https://www.seeedstudio.com/Seeed-XIAO-BLE-Sense-nRF52840-p-5253.html)
- **Issues**: [tooling](https://github.com/Jdubz/blinky_time/issues), [firmware](https://github.com/Jdubz/blinky-firmware/issues)
