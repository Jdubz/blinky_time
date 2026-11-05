# Contributing to Blinky Time (tooling)

This repo is the tooling: blinky-server, blinky-console, ml-training, blinky-serial-mcp, the test box scripts, fleet deploys and device configs (#357).

- **Firmware** (`blinky-things/`, bootloader, simulator, host tests) goes to **[blinky-firmware](https://github.com/Jdubz/blinky-firmware)**, by PR to its `staging` branch. That repo's [CONTRIBUTING.md](https://github.com/Jdubz/blinky-firmware/blob/staging/CONTRIBUTING.md) covers building, test devices and the hardware-tested release flow.
- **Labels and scoring** (`edm/`, `blinky_scoring`) go to **[blinky-corpus](https://github.com/Jdubz/blinky-corpus)**, by PR to `main`.

A change that crosses repos (a wire protocol, a firmware tool the server runs, a new label file) is a PR in each. This repo picks up the other two through its `firmware/` and `corpus/` submodule pins, which Dependabot bumps by PR.

## Code of Conduct

By participating in this project, you agree to maintain a respectful and inclusive environment for all contributors.

## Getting started

- Git, Python 3.11+, Node 20+ (for the console), bash (Git Bash on Windows).
- `git clone --recurse-submodules https://github.com/Jdubz/blinky_time.git`, or `git submodule update --init firmware corpus` in an existing clone.
- **Read [CLAUDE.md](CLAUDE.md) before touching hardware.** It holds the flashing, release and test box rules. In short: never use `arduino-cli upload`, the Arduino IDE Upload button or `adafruit-nrfutil dfu serial` on the nRF52840, and only stable releases reach fleet devices, through `scripts/deploy.sh`.

Setup and tests for each part:

| Part | Setup | Tests |
|------|-------|-------|
| blinky-server | `cd blinky-server && python -m venv venv && venv/bin/pip install -e '.[dev,ble]'` | `venv/bin/python -m pytest tests/` |
| blinky-console | `cd blinky-console && npm install` (also installs the git hooks) | `npm run test`, `npm run lint` |
| test box scripts | the server's venv | `pytest scripts/testbox/test_deploy.py scripts/testbox/test_staging.py` |
| ml-training | see `ml-training/README.md` | `python -m pytest tests/` in `ml-training/` |

## Workflow

All work branches from `master` and merges back by pull request. See [.github/BRANCHING_STRATEGY.md](.github/BRANCHING_STRATEGY.md).

1. Branch from `master`: `fix/...`, `feat/...`, `docs/...`, one change per branch.
2. Open a PR to `master`. CI (`.github/workflows/pr-validation.yml`) must pass: the server, console and test box tests, Python ↔ firmware feature parity, the trainer's Spark contract, the ADTOF labeler and the docs check. The Claude review and Copilot review run on every PR. Address what they find.
3. Merged changes that touch the server's paths reach the test box through its deployer, after CI is green and the PR's last commit has a clean Claude review verdict (CLAUDE.md, "The test box's blinky-server runs this repo's merged master only").

## Guidelines

- **No silent fallbacks**: invalid state fails loudly (CLAUDE.md, "No Silent Fallbacks").
- **Long server jobs are durable workflows** with a restart test (CLAUDE.md, "blinky-server work survives any restart").
- **Device configs** are JSON in `devices/registry/`, uploaded at runtime, never compiled in.
- **Docs** only with future value: plans, specs, guides, outstanding-action lists (CLAUDE.md, "Documentation Guidelines"). Index: [docs/README.md](docs/README.md).
- **Commit messages**: say what changed and why; reference the issue.

## Bug reports

Firmware bugs (what a device does) go to [blinky-firmware's issues](https://github.com/Jdubz/blinky-firmware/issues); tooling bugs here. Include:

- Device ID and firmware version (`json info`), bootloader version if relevant.
- What you did, what happened, what you expected.
- Server logs or serial output, photos or video if they help.

## License

By contributing, you agree that your contributions will be licensed under the Creative Commons Attribution-ShareAlike 4.0 International License.
