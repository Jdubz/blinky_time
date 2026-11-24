# Test box

The test box is a Raspberry Pi 5 (`blinkytest`) with one test chip, a speaker and a camera. It runs blinky-server from merged master, hardware-tests blinky-firmware's `staging` head, and recovers its chip without anyone touching it. It is a test box, not a strict CI box (owner, 2026-11-23): every chip on it is a test chip (#382).

The box routes data and captures tests. Heavy work (training, stem separation, label generation, long analysis) goes to CI or to the DGX Spark through its job API, not to the Pi.

## Two hosts, two roles

| | blinkyhost (production) | Test box (`blinkytest`) |
|---|---|---|
| Runs | lemon-cart (sibling repo): WiFi AP, Caddy, Hub UI, station-api, blinky-server | blinky-server only (`--no-ble --staging`), including its Spark client (#288): it submits GPU jobs and receives the Spark's webhooks. Large assets stay in the Spark's object store; the box keeps only data it collects and test files. |
| Manages | Finished devices in the fleet | Its own test chip |
| Firmware | Latest **stable** only, through `./scripts/deploy.sh` | Staging builds (the staging suite) and stable releases (`POST /api/fleet/deploy`) |
| Audio | Station audio (PipeWire) for guests | The box speaker, validation corpus only |
| Validation runs | None | All of them |

Rules that follow:

- **The two servers never share devices.** A finished device is never plugged into the box, and a box chip is never enrolled on blinkyhost.
- **No BLE on the box server.** It runs with `--no-ble`, and `setup.sh` turns Bluetooth off at the OS level (`dtoverlay=disable-bt`). blinky-server has no discovery allowlist, so with BLE on it could adopt a production device in radio range, and its auto-recovery could flash it.
- **No validation on blinkyhost.** A validation run plays music through the host's audio output, which on blinkyhost is the station's sound system.
- **`deploy.sh` stays production-only.** It targets `blinkyhost.local:8420` and ships only stable releases.
- **Every chip on a `--staging` server is a test chip** (#382). `device upload`, `reboot` and the rest of the gated list go straight to one chip through `/api/devices/{id}/command`, with no `X-Deploy-Tool` header; fleet-wide broadcasts stay gated. Staging builds go to any configured chip on the box, with no test-device list. The 60-second rule between resets still applies (CLAUDE.md).

## Hardware as built

Owner-stated and checked on 2026-11-23. Don't change this section from guesses; ask.

| Part | As built |
|------|----------|
| Host | Raspberry Pi 5, hostname `blinkytest`. |
| Test chip | One XIAO nRF52840 **Sense Plus**, USB serial `87DE9B58C90AFE2D`, device config `testbox_grid_v1` (listed in blinky-firmware's `devices/test_devices.json`). All XIAO pins are soldered. Every bootloader and firmware build must run on both the Sense Plus and the fleet's plain Sense. |
| SWD probe | Seeed **XIAO Debug Mate**, the chip seated in it. CMSIS-DAP driven with OpenOCD, used for SWD recovery only. |
| USB hub | Belkin hub with per-port power switching (`uhubctl`). The chip's own USB-C and the Debug Mate's USB-C are each on a hub port. |
| LEDs | An 8x8 WS2812 grid, data from **D10** through the Debug Mate's D10 header pin (a direct trace to the socket in Seeed's netlist). The grid has its own external 5 V supply. No level shifter. |
| Grid wiring | Data enters at the bottom-right LED (seen from the front); each row runs right to left, rows go bottom to top, no serpentine. Firmware orientation 5, `HORIZONTAL_ROTATED_180` ([blinky-firmware#46](https://github.com/Jdubz/blinky-firmware/pull/46), merged, not yet in a stable release). |
| Camera | OmniVision OV5647 (Camera Module v1 class) on the Pi's CSI port. Rolling shutter, fixed focus; 640×480 binned at up to 67.5 fps (measured 2026-10-27). Capture and analysis: `scripts/testbox/camera/` (system Python, `python3-picamera2`); the plan and bench numbers: `TEST_BOX_CAMERA_HARNESS_PLAN.md`. |
| Speaker | JBL Pebbles (USB-powered, USB audio, ALSA card name `Pebbles`), plugged into the Pi directly, not into the switched hub, so a port cycle or hub reset can't cut it mid-run. |

Firmware-side details of the chip and grid: blinky-firmware's [HARDWARE.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/HARDWARE.md).

### XIAO Debug Mate: what to know

From its schematic and firmware; details and sources in blinky-firmware's [SWD_RECOVERY.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/SWD_RECOVERY.md).

- CMSIS-DAP v1 (HID) on an ESP32-S3, driven with OpenOCD. pyOCD can't open it (its USB serial is empty).
- **No working nRESET.** Every reset is SYSRESETREQ or CTRL-AP. Never configure `reset_config srst_*`.
- **One OpenOCD at a time.** Two sessions, or one killed mid-transaction, wedge the probe. `swd_recover.sh` refuses to run if an OpenOCD is alive.
- It powers the chip from its 5V pin, so the chip runs even with its own USB cable unplugged.
- Its GND at the socket is the low side of its power-analyser shunt, and it is rated 5 V / 1 A. Never power LEDs through it.
- Debugging works only while its screen shows the main dashboard page.

## Recovery chain (hands-off)

1. **USB, by blinky-server** (#378, #379). A chip enumerating as its UF2 bootloader (`2886:0045`, or `0065` for a Sense Plus factory bootloader) is parked in `DFU_RECOVERY`. After 120 s there, the server flashes the armed stable image through the single flash path onto the drive bound by USB serial, then verifies. Recovery is armed for a device by a stable deploy to it through `POST /api/fleet/deploy`. Verified on 2026-11-21: a chip with no app came back on b217 with nobody touching it.
2. **SWD:** blinky-firmware's `scripts/swd_recover.sh restore` (backup, CTRL-AP erase, MBR + S140 + fleet bootloader + UICR + stable app + a valid settings page, verify, reset). Verified in 3.5 min. The server doesn't trigger it yet (open work in blinky-firmware's [BOOTLOADER_WORKFLOW.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/BOOTLOADER_WORKFLOW.md)).
3. **Power:** the hub cuts VBUS per port. A real cold boot cuts **both** ports, the chip's and the Debug Mate's, because the Debug Mate also powers the chip. End SWD sessions with the core running: a core left halted survives a cut of the chip's port alone.

Two behaviours to expect: the server doesn't auto-recover a device within 30 minutes of a UF2 write to it (the cascade guard), and a chip stuck in an enumeration loop can make the hub reset its upstream link, so switch that port off.

## Software setup

1. **Provisioning.** Flash Raspberry Pi OS Lite 64-bit with Raspberry Pi Imager (hostname `blinkytest`, user `jdubz`). Copy `scripts/testbox/` to the card's boot partition as `blinky-testbox/`, add your SSH key under `user:` → `ssh_authorized_keys`, and append `- [ bash, /boot/firmware/blinky-testbox/setup.sh, --user, jdubz ]` to `runcmd` in `user-data`. On first boot cloud-init runs `setup.sh` (log: `/var/log/blinky-testbox-setup.log`; result: `/var/lib/blinky-testbox/setup-ok` or `setup-failed`). It is idempotent: `sudo ~/blinky_time/scripts/testbox/setup.sh` re-runs it.
2. **What `setup.sh` installs** (its header lists every step): the packages (Python, `ffmpeg` for `ffplay`, `uhubctl`, `udisks2`, `gh`, Node, OpenOCD); the repo clone; blinky-server with the `firmware` and `ble` extras (the server imports bleak at startup; BLE stays off through `--no-ble` and the overlay); the console build; passwordless sudo for the user (`sudoers.in`: the box is a dedicated dev/test machine administered by the Claude session that develops on it); Bluetooth off (`dtoverlay=disable-bt`); a udev rule for the Debug Mate; the deploy worktrees and units (step 3); and the development tooling, pinned as CI pins it (arduino-cli and the firmware toolchain, cppcheck, GCC 13 as `$CXX`, adafruit-nrfutil, blinky-server's `dev` extra, a dev venv `~/.venvs/blinky`, blinky-serial-mcp registered with Claude Code against the box's server, git hooks, Claude Code). `ml-training/requirements.txt` is not installed: training runs on the Spark. The server creates its API key in `~/.blinky-api-key` on first use. Manual afterwards: `gh auth login`, `gh auth setup-git`, your git `user.name`/`user.email`, and sign in to Claude Code.
3. **Deploys come from master only (blue/green).** blinky-server runs from `~/blinky-deploy`, a symlink to one of two worktrees of the clone, `~/blinky-deploy-a` and `~/blinky-deploy-b`. Each is detached at a master commit, with its own venv and console build. The clone `~/blinky_time` is for development; the server never runs from it.

   `blinky-deploy.timer` runs `scripts/testbox/deploy.py` every 2 minutes. It takes a new master commit only when:
   - it is the merge of a PR into master;
   - every check run on the master commit is green, including `Blinky Server Tests`;
   - the PR's last commit has a clean Claude review: the workflow's sha-stamped verdict comment, posted by `github-actions[bot]`, says "No issues found" (the `claude-review` check alone is green even when the review found issues).

   A commit still in CI waits for the next pass. A red one, or one whose review has findings, is refused. `deploy.py --accept SHA "reason"` deploys a refused commit anyway, as the owner's decision recorded in the state.

   **Only server changes restart the server.** `SERVER_PATHS` in `deploy.py` lists what the server reads or runs from its tree: `blinky-server/`, `blinky-console/`, `scripts/testbox/`, and the `firmware` and `corpus` submodules (the firmware's `tools/` and the corpus with its scorers). A commit that passes the gate but touches none of these only moves `refs/blinky/deployable`, which the Spark training bundle is built from; nothing restarts.

   A server change is installed into the idle slot while the server keeps running. The venv is reinstalled only if `blinky-server/pyproject.toml` changed, the console rebuilt only if `blinky-console/` changed, and the slot's systemd units installed where they changed. Then the deployer stops the server, which drains (#325, `blinky_server/work/durable.py`): it refuses new work, lets every step or flash in flight finish, holds each durable job at its next step boundary, and exits. `KillMode=mixed` keeps the SIGTERM off the ffplay or UF2 copy the server is waiting for, and systemd's stop timeout is held open while it drains. Only then does the link move and the new server start. If `/api/devices` doesn't answer within 90 s, the deployer switches back and refuses the commit. `GET /api/work` shows what's in flight.

   - First install: `setup.sh` runs `deploy.py --init`.
   - State: `python3 ~/blinky-deploy/scripts/testbox/deploy.py --status` (`--dry-run` gates the latest master and changes nothing).
   - Log: `journalctl -u blinky-deploy`.
   - Never restart the server against the dev clone. To try a branch, run it on another port from `~/blinky_time`.
4. **USB speaker as the default ALSA device.** `audio_player.py` uses default ALSA routing (dmix), not `AUDIODEV=hw:N,0`, and Pi OS Lite has no PipeWire, so without this ffplay plays to HDMI. Put the card's name (from `aplay -l`, the word after `card N:`) in `/etc/asound.conf` or the server user's `~/.asoundrc`, by name and with the `!` override (USB card numbers can change between boots, and a plain `defaults.pcm.card <name>` is rejected as "card is not a string"):
   ```
   defaults.pcm.!card Pebbles
   defaults.ctl.!card Pebbles
   ```
   Check with `ffplay -nodisp -autoexit <track>.mp3`.
5. **Enroll a chip** with its device config through `device upload` (on the box, straight to the chip, #382), and respect the 60-second rule between resets.

## Staging runner

`blinky-staging.timer` runs `scripts/testbox/staging.py` every 2 minutes from the deploy worktree. It hardware-tests the newest commit on blinky-firmware's `staging` branch: it submits the staging suite (`POST /api/test/staging`, `blinky_server/testing/staging.py`) for every connected, configured chip on the box, then posts the "Hardware test (staging)" commit status and the report on the PR. On a pass it opens the staging → master PR and turns on its auto-merge. A commit whose firmware image is unchanged (same fingerprint, no change under the firmware's paths) is skipped and promoted. State: `staging.py --status`; log: `journalctl -u blinky-staging`. The release flow around it is in blinky-firmware's [RELEASE_PROCESS.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/RELEASE_PROCESS.md).

The suite deploys the staging build through the deploy pipeline, checks health (fps floor, longest frame, no reboot, overrun growth: release_gate's checks), plays every edm/ track once for validation and the music/beat-pace checks, then checks the version is unchanged, no reboot, and no firmware `[ASSERT]`/`[FALLBACK]`. The validation numbers are reported, not gated, because the box's numbers aren't trusted as a floor yet (see "Trusting the box's numbers" below).

blinky-firmware's `tools/deploy_local.py` and `tools/release_gate.py` (and `devices/test_devices.json`) live in blinky-firmware and run against a chip on the machine's own USB. On the box blinky-server owns the chip's port, so the staging suite is how the box tests firmware.

## Needed: per-server device ownership

`--no-ble` keeps the box from adopting production devices only by turning off a transport. The real requirement is that **every device belongs to exactly one server, and a server never touches a device it doesn't own**, whatever the transport or radio range. Proposed shape (to be designed in its own issue):

- **An ownership manifest per server** listing each owned device's ID, BLE MAC and USB serial.
- **Refuse what isn't listed.** No connecting to, adopting, commanding, auto-recovering or flashing a device outside the manifest; log every refusal. Unknown devices show as "foreign", read-only.
- **Check identity on connect.** `json info` `device.id` must match the manifest entry for that MAC/serial; a mismatch is a loud error.
- **One owner per device.** A check fails if a device is in both manifests, or a production device is in blinky-firmware's `devices/test_devices.json`.
- **Enrolling a chip** is an explicit, logged step, never something discovery does on its own.

BLE testing in the box waits on this.

## Open questions

- Should the box test BLE (fleet commands, BLE DFU)? It first needs per-server device ownership.
- Should the server trigger SWD recovery itself when USB recovery fails (blinky-firmware's `BOOTLOADER_WORKFLOW.md`)?

## Ideas (not decided)

Written before the box was built, without an owner request. None of this is a requirement; take any of it up only with the owner.

- **Trusting the box's numbers.** Checks that would let the staging suite gate on validation numbers: host timing (spread of the estimated audio latency over 20 plays of one track, no throttling, logged with `vcgencmd`); repeatability (same firmware and track, 5 runs, small onset-score spread); agreement with earlier bench results for the same build.
- **Acoustic isolation.** An enclosure would make runs independent of room noise. Size, materials and construction are open.
- **Speaker bass.** The Pebbles is small and ported; a sweep would show how well it reproduces kick fundamentals (~50 Hz).
- **Calibration gear.** A measurement mic and an SPL meter to set and record the playback level at the chip.
- **Grid data line.** A series resistor on the data line and a bulk capacitor at the grid's 5 V input are common WS2812 practice; a level shifter if flicker ever appears.
