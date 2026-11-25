# blinky-serial-mcp

An MCP server for AI-assisted testing and tuning. It is a thin HTTP client of blinky-server: every device tool is a REST call to the server (`http://localhost:8420/api/...`), which owns the serial ports.

```bash
npm ci && npm run build
node dist/index.js        # stdio; the test box's setup.sh registers it with Claude Code
```

## Environment

| Variable | Used by | Meaning |
|---|---|---|
| `BLINKY_TRACK_DIR` | validation suites | The track directory on the machine **blinky-server** runs on. Default: this repo's `corpus/edm`. |
| `BLINKY_SIMULATOR` | `render_preview` | blinky-firmware's built simulator binary. Unset, the tool returns an error saying so. |

`render_preview` runs [blinky-firmware](https://github.com/Jdubz/blinky-firmware)'s simulator (its `blinky-simulator/`), which this repo doesn't build or ship. To use it, build the simulator in a blinky-firmware checkout and point the variable at the binary:

```bash
cd <blinky-firmware>/blinky-simulator && ./build.sh
export BLINKY_SIMULATOR=<blinky-firmware>/blinky-simulator/build/blinky-simulator
```

The GIFs and `metrics.json` go to the tool's `output_dir`, or to `blinky-simulator/previews/` beside the binary's `build/`.
