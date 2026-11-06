# Development Guide - Blinky Time (tooling)

This guide covers the tooling in this repo: blinky-server, blinky-console and the test box scripts. Firmware development (platform code, `SETTINGS_VERSION`, config safety, the firmware review checklist) is in [blinky-firmware's DEVELOPMENT.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/DEVELOPMENT.md). Setup and tests for each part: [CONTRIBUTING.md](../CONTRIBUTING.md).

## First-time setup

Clone with the submodules (`git clone --recurse-submodules`, or `git submodule update --init firmware corpus`). Then install the git pre-push hook so blinky-server ruff (lint + format) and mypy run before every push. Without this, format-only failures only get caught on CI, costing a round-trip.

```bash
./scripts/install-hooks.sh
```

The hook source is in `hooks/pre-push`; the script copies it into `.git/hooks/`. It also warns when the `.git` pack exceeds 500 MB. Bypass for a single push with `git push --no-verify` (use sparingly — every CI failure that the hook would have caught is a wasted round-trip).

The console has a separate husky setup; `cd blinky-console && npm install` installs its hooks. See `README.md`.

## Flashing

Fleet devices get firmware only through `./scripts/deploy.sh --devices=<id|all|list>` (the latest stable release; CLAUDE.md, "Upload Safety"). A device on your own USB goes through blinky-firmware's `tools/deploy_local.py`.

---

## 🌐 Console + Server Development

The `blinky-console` (React/Vite SPA) and `blinky-server` (FastAPI fleet
manager) can run independently or together.

### Standalone console (WebSerial only)

```bash
cd blinky-console && npm run dev
```

Opens at `http://localhost:3000`. Talks WebSerial directly to plugged-in
devices. No server needed.

### Console + server together

```bash
# Terminal 1: server (port 8420)
cd blinky-server && ./venv/bin/python -m blinky_server

# Terminal 2: console (port 3000, proxies /api and /ws to :8420)
cd blinky-console && npm run dev
```

Console at `:3000` keeps HMR + WebSerial; `/api/*` and `/ws/*` requests
proxy through to the server. Edit either side without rebuilding.

### Production-style: server serves the SPA

```bash
cd blinky-console && npm run build       # outputs to ../blinky-server/web/
cd blinky-server && ./venv/bin/python -m blinky_server
```

Server at `http://localhost:8420` serves both the API and the SPA from
the same origin. Override the static dir location with the
`BLINKY_STATIC_DIR` env var if needed.
