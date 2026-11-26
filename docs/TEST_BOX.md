# Test box

The test box is a Raspberry Pi 5 with one test chip, a speaker and a camera. It runs blinky-server from merged master, runs the blocking hardware checks on every firmware PR to blinky-firmware's `staging` and confirms them for the staging head, scores the staging head with the non-blocking quality suite, and recovers its chip without anyone touching it. It is a test box, not a strict CI box (owner, 2026-11-23): every chip on it is a test chip (#382).

The box routes data and captures tests. Heavy work (training, stem separation, label generation, long analysis) goes to CI or to the DGX Spark through its job API, not to the Pi.

## Two hosts, two roles

| | Fleet server (production) | Test box |
|---|---|---|
| Runs | lemon-cart (sibling repo): WiFi AP, Caddy, Hub UI, station-api, blinky-server | blinky-server only (`--no-ble --staging`), including its Spark client (#288): it submits GPU jobs and receives the Spark's webhooks. Large assets stay in the Spark's object store; the box keeps only data it collects and test files. |
| Manages | Finished devices in the fleet | Its own test chip |
| Firmware | Latest **stable** only, through `./scripts/deploy.sh` | Staging builds (the staging suite) and stable releases (`POST /api/fleet/deploy`) |
| Audio | Station audio (PipeWire) for guests | The box speaker, validation corpus only |
| Validation runs | None | All of them |

Rules that follow:

- **The two servers never share devices.** A finished device is never plugged into the box, and a box chip is never enrolled on the fleet server.
- **No BLE on the box server.** It runs with `--no-ble`, and `setup.sh` turns Bluetooth off at the OS level (`dtoverlay=disable-bt`). blinky-server has no discovery allowlist, so with BLE on it could adopt a production device in radio range, and its auto-recovery could flash it.
- **No validation on the fleet server.** A validation run plays music through the host's audio output, which on the fleet server is the station's sound system.
- **`deploy.sh` stays production-only.** It targets the fleet server (`BLINKY_FLEET_URL`, private config below) and ships only stable releases.
- **Every chip on a `--staging` server is a test chip** (#382). `device upload`, `reboot` and the rest of the gated list go straight to one chip through `/api/devices/{id}/command`, with no `X-Deploy-Tool` header; fleet-wide broadcasts stay gated. Staging builds go to any configured chip on the box, with no test-device list. The 60-second rule between resets still applies (CLAUDE.md).

## Hardware as built

Owner-stated and checked on 2026-11-23. Don't change this section from guesses; ask.

| Part | As built |
|------|----------|
| Host | Raspberry Pi 5. Its hostname is in the owner's private notes; Alloy labels the box's telemetry with it (`host`). |
| Test chip | One XIAO nRF52840 **Sense Plus**, USB serial `87DE9B58C90AFE2D`, device config `testbox_grid_v1` (listed in blinky-firmware's `devices/test_devices.json`). All XIAO pins are soldered. Every bootloader and firmware build must run on both the Sense Plus and the fleet's plain Sense. |
| Second chip | A **XIAO ESP32-S3**, attached by the owner on 2026-11-24 (USB cables replaced the same day). Measured: native USB Serial/JTAG, MAC `90:70:69:11:F8:10`, `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_90:70:69:11:F8:10-if00`; it runs an old blinky build (`1.0.1`), **unconfigured, in safe mode**; blinky-server connects to it (`esp32s3`) and `json info` answers in ~2-16 ms. LED data on **D0** (owner, 2026-11-24); the rest of its LED wiring (LED type, count, layout, power) and its device config are not recorded yet: ask the owner. The box has no ESP32 flash path yet (P3). |
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

## LEDs: dark unless a test runs

The grid is lit only while a test runs (owner, 2026-11-24). Only the box's chip works this way; every fleet device keeps booting lit, in Fire, at its configured brightness.

- **Idle and at boot: dark.** The chip's device config, `devices/registry/testbox_grid_v1.json`, has `brightness: 0`. A reboot, a flash or a deploy leaves it dark.
- **During a run: lit at 40.** Test runs light the chip with the firmware's runtime-only `leds` override (blinky-firmware `render/LedBrightness.h`; `leds 0-255 | off | config | status`, never saved), at `TEST_BRIGHTNESS` (`blinky_server/testing/leds.py`): the brightness the camera's exposure and gain were set for (`TEST_BOX_CAMERA_HARNESS_PLAN.md`, "Levels"). It is a box setting, not a device one: any chip in the grid's place needs the camera's level.
- **After a run, however it ends** (pass, failure, cancel, a drain's stop): `leds config`, back to the config's brightness (dark).
- **Restarts.** Lighting is per-run setup in the workflow body, so a job recovered after a restart lights the chip again. When the chip (re)connects, the server sets it back to its config brightness unless a run in this process holds it lit, so a run cut off by a crash doesn't leave it lit.
- **Only the box's chips.** The server sends `leds` only on a `--staging` server and only to configured chips (not the unconfigured ESP32-S3). The API refuses `leds` on any other server (command routes, WebSocket commands, test jobs' command lists), and in fleet broadcasts everywhere; a command with a CR or LF in it is refused everywhere, so a second line can't slip past the gates.

Per run type:

| Run | Lit | Needs light | On firmware without `leds` |
|---|---|---|---|
| Hardware checks (`test.hardware-checks`) | from the deploy's end (or the job's start, in dev mode) to the job's end | the lights check does: it sets `leds` itself and judges that it applies | the lights check answers no |
| Staging quality suite (health, validation, music and beat pace, final state) | from the deploy's end to the job's end | no: it reads the chip over serial; lit and dark frames cost the same (the override scales pixel values; generators, renderer and LED DMA run either way) | an ERROR in the server log; the run goes on |
| Validation, parameter sweep, threshold tuning | for the test session | no | an ERROR in the log; the run goes on |
| Camera review (`camera_review.py`: validation with `needs_light`) | for the test session | yes | the job fails (so does a planned chip that isn't connected when the session starts: it would join dark) |
| Camera calibration (`camera_calibrate.py`, `--brightness`, default 40) | for the script | yes | the script refuses |

A camera survey (`camera_capture.py survey`) runs outside any job: light the chip first (`leds 40` through `POST /api/devices/{id}/command`) and send `leds config` after.

## Audio start

The drum scorer removes each run's lag, from the audio start to the chip's detections, before matching (blinky-corpus `blinky_scoring/lag.py`). The server measures that start instead of guessing it.

**Where the old lag came from.** Up to 2026-11-25 the server timed the run from spawning ffplay (`audio_start_epoch_ms`), and the detections came 240-318 ms later. In 28 runs, the speaker's ALSA substream was read from `/proc/asound` during live jobs, next to the scorer's lag:

| part | measured (28 runs, 2026-11-25) |
|---|---|
| spawn to the ALSA stream starting (ffplay exec, probe, seek, open) | 113-163 ms, median 134, sd 13 |
| stream start to the chip's detection | 117-171 ms, median 149, sd 16, in steps of about 21 ms |
| the clock sync putting detections on the host clock (#394) | about 3 ms |

The steps in the second part are ffplay: its SDL callback hands out silence, a chunk at a time, until the decoder has the seeked audio. So the music started 0-2 chunks after the stream did. Both parts vary run to run, and neither is the speaker or the chip.

**The method.** The player (`blinky-server/blinky_server/testing/alsa_play.py`) decodes exactly the segment with ffmpeg and writes it to ALSA (`plughw` on the default card) through libasound with ctypes: no new package. After each write it takes `snd_pcm_status`. With timestamp T, delay D (frames queued ahead of the DAC) and W frames written, the first frame was at the DAC at `T - (W - D) / rate`. It uses the median of the first 40 such estimates and reports their spread. Sources: the kernel's timestamping design (`Documentation/sound/designs/timestamping.rst`: the delay is "the time it will take to hear a new sample after all queued samples have been played out"; USB audio dates its position with the bus frame counter, to about 1 ms), and alsa-lib's `snd_pcm_delay` ("the overall latency from the write call to the final DAC"). PortAudio's `outputBufferDacTime` is the same quantity, computed the same way on ALSA. dmix's delay leaves out the frames queued on the USB bus, which is why the player uses `plughw`. The audio lock keeps any other player off the device.

**What the measurement can't see:** the speaker's own processing, the air, and the chip's mic buffering and detection. All of these are fixed for a rig and a firmware. The server adds them as one calibrated constant, `audio_player.AUDIO_PATH_MS` (override: `BLINKY_AUDIO_PATH_MS`). The run is then scored at `lag = (DAC start - spawn) + AUDIO_PATH_MS`, its drum score says `lagSource: measured`, and the scorer's search over the music becomes the cross-check (`lagCheckMs`, `lagNote`). A chip whose detection latency differs from the calibrated one shows up as `lagCheckMs - lagMs`. This follows the evaluation convention of removing a known delay as a fixed, calibrated constant (madmom's `delay`; Böck et al., ISMIR 2012).

**Calibration (2026-11-25, firmware `b211-dev-c5f4d51a`, 28 runs: techno-machine-drum ×5, techno-minimal-01 ×5, edm/ ×1):**

| | measured |
|---|---|
| the player's spawn to the first sample at the DAC (`dac_offset_ms`) | 155-198 ms, median 174, sd 11: Python and ffmpeg start-up, measured each run |
| the estimates' spread within a run | 0.86-0.99 ms (one run 4.0 ms); no underruns, no fallbacks |
| `AUDIO_PATH_MS` = the scorer's lag minus the DAC offset, on the 14 runs it resolves | 42.0-49.7 ms, median 45.2, sd 2.5 (the 14 ambiguous runs: 41-50) |
| the music's check against the measured lag (`lagCheckMs - lagMs`), all 28 runs at 45 ms | -4.2 to +4.9 ms, sd 2.7 |

**Detection latency, watched.** The fixed path includes the chip's own detection latency on purpose: a firmware that detects later loses score instead of having it searched away. Every report that shows scores (the staging quality suite, `dev.py` validate runs, `/api/test/validate`) gives each track's `lagCheckMs − lagMs` and the median over the run's tracks (`detection_latency` in the report; metric `blinky_detection_latency_offset_ms{device,source}`), and the strict rule `detection_latency` warns when that median leaves ±10 ms. Only tracks whose lag was measured and whose kickR50 is at least 0.5 count: on sparser tracks the music's check locks onto the few matches it finds (at calibration dubstep-edm-halftime, kickR50 0.25, read +57 ms and edm-trap-electro, 0.31, read −16 ms, while the other 26 runs had kickR50 ≥ 0.58 and sat within −6.1..+6.6 ms, sd 2.9). Tracks left out are listed with the reason. The ±10 ms is 3.4 sd of a single track, so a one-track run of the calibrated firmware stays inside, and a third of the tight ±30 ms window, so a drift warns before it costs kickR30. A drift past ±50 ms would also lower kickR50 and could leave tracks out; it then shows as the scorer's own `lagNote` and as falling scores. A persistent offset with no firmware change means recalibrating (below); with a firmware change it is that firmware's latency.

To recalibrate after a change of speaker, placement or detection latency: run validation with `persist_raw`, and take the median of `drums.lagCheckMs` minus `audio_start.dac_offset_ms` (the music's lag near the measured one). For a change larger than the check's ±100 ms, start the server with `BLINKY_AUDIO_PATH_MS=none`, which turns the measured lag off so every lag is searched. Then take the median of `lagMs` minus `dac_offset_ms` on the runs the scorer resolves (`lagAmbiguous` false).

**Recorded:** every raw capture keeps `audio_start_epoch_ms` (the spawn: every `t` is measured from it) and adds `audio_dac_start_epoch_ms` and `audio_start` (offset, spread, the lag used, or why there was none). Every validation result carries `audio_start`, and every score carries `audioStart`.

**Failures are explicit.** If the player can't play anything (no libasound, the device won't open), ffplay plays the file as before. A timestamp spread over 10 ms, an underrun (the audio after it is late by the gap) or a missing calibration leaves the audio playing but the start untrusted. In all these cases the run isn't measured: `audioStart.measured` is false with the reason, the scorer searches as before, and the server logs a `[FALLBACK]` line.

## Recovery chain (hands-off)

1. **USB, by blinky-server** (#378, #379). A chip enumerating as its UF2 bootloader (`2886:0045`, or `0065` for a Sense Plus factory bootloader) is parked in `DFU_RECOVERY`. After 120 s there, the server flashes the armed stable image through the single flash path onto the drive bound by USB serial, then verifies. Recovery is armed for a device by a stable deploy to it through `POST /api/fleet/deploy`. Verified on 2026-11-21: a chip with no app came back on b217 with nobody touching it.
2. **SWD:** blinky-firmware's `scripts/swd_recover.sh restore` (backup, CTRL-AP erase, MBR + S140 + fleet bootloader + UICR + stable app + a valid settings page, verify, reset). Verified in 3.5 min. The server doesn't trigger it yet (open work in blinky-firmware's [BOOTLOADER_WORKFLOW.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/BOOTLOADER_WORKFLOW.md)).
3. **Power:** the hub cuts VBUS per port. A real cold boot cuts **both** ports, the chip's and the Debug Mate's, because the Debug Mate also powers the chip. End SWD sessions with the core running: a core left halted survives a cut of the chip's port alone.

Three behaviours to expect: the server doesn't auto-recover a device within 30 minutes of a UF2 write to it (the cascade guard); a flash whose UF2 drive is still attached 60 s after the copy (the bootloader never got every block, as when a USB reset cut the copy short on 2026-11-25) copies once more into that bootloader, with the anomaly `uf2_write_retried`, instead of waiting out the guard; and a chip stuck in an enumeration loop can make the hub reset its upstream link, so switch that port off.

## Software setup

1. **Provisioning.** Flash Raspberry Pi OS Lite 64-bit with Raspberry Pi Imager (the box's hostname from the owner's private notes, user `jdubz`). Copy `scripts/testbox/` to the card's boot partition as `blinky-testbox/`, add your SSH key under `user:` → `ssh_authorized_keys`, and append `- [ bash, /boot/firmware/blinky-testbox/setup.sh, --user, jdubz ]` to `runcmd` in `user-data`. On first boot cloud-init runs `setup.sh` (log: `/var/log/blinky-testbox-setup.log`; result: `/var/lib/blinky-testbox/setup-ok` or `setup-failed`). It is idempotent: `sudo ~/blinky_time/scripts/testbox/setup.sh` re-runs it.

   **Private config.** This repo is public, so host names, addresses and keys live in `~/.config/blinky/` (the directory mode 700, its files mode 600), never in git. Each file is `KEY=value`, parsed and never sourced; an environment variable of the same name overrides the file, and a tool that needs a missing key fails, naming the file and the key. The values are in the owner's private notes. The files and their keys:
   - `hosts.env` (template `scripts/hosts.env.example`): `BLINKY_FLEET_URL` (the fleet server, for `deploy.sh`), `BLINKY_TESTBOX_URL` (this box as the home network reaches it, for the staging report's links; setup.sh's host config step and `staging.py` stop, naming it, until it's set), `BLINKY_SERIAL_SSH` (the fleet server's ssh target for `.mcp.json`; export it in your shell, because Claude Code reads only the environment).
   - `observability.env` (template `scripts/testbox/observability.env.example`): `LOKI_PUSH_URL`, `PROM_RW_URL`, `TEMPO_OTLP_ENDPOINT`, the LGTM stack's push endpoints. Until the file exists, setup.sh's Alloy step fails and names the file and keys.
   - `spark.env`: `SPARK_JOB_API`, `SPARK_API_KEY`, `SPARK_WEBHOOK_SECRET` (`blinky_server/spark/service.py`).
   - `build.env`: `BLINKY_BUILD_URL`, `BLINKY_BUILD_TOKEN` (the build service, `DEV_LOOP.md`).

   On first boot, an `observability.env` or `hosts.env` next to `setup.sh` on the card seeds the file; delete the copy afterwards. `scripts/check_topology.py` (CI's Documentation Check) fails on a host name or private address in the repo; a justified exception goes in `.topology-allowlist` with its reason.
2. **What `setup.sh` installs** (its header lists every step): the packages (Python, `ffmpeg` for `ffplay`, `uhubctl`, `udisks2`, `gh`, Node, OpenOCD); the repo clone with its `corpus` submodule; blinky-server with the `firmware` and `ble` extras (the server imports bleak at startup; BLE stays off through `--no-ble` and the overlay); the console build; passwordless sudo for the user (`sudoers.in`: the box is a dedicated dev/test machine administered by the Claude session that develops on it); Bluetooth off (`dtoverlay=disable-bt`); a udev rule for the Debug Mate; the deploy worktrees and units (step 3); and this repo's development tooling (blinky-server's `dev` extra in its own venv, a managed `~/.profile` block that puts `~/.local/bin` on PATH and nothing else, blinky-serial-mcp registered with Claude Code against the box's server, git hooks, Claude Code). No dev venv and no `ml-training/requirements.txt`: ml analysis and training run on CI or the Spark, not the box, and nothing here compiles C++. blinky-server's tests run in its venv (`blinky-server/venv`). No firmware toolchain (arduino-cli, the pinned core, cppcheck, GCC 13, adafruit-nrfutil): the box flashes published images with blinky-server's own host tools (`blinky-server/hosttools/`). To develop firmware on the box, clone blinky-firmware and follow its [BUILD_GUIDE.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/BUILD_GUIDE.md) (arduino-cli at the pinned version, then `bash scripts/setup_toolchain.sh`). The server creates its API key in `~/.blinky-api-key` on first use. Manual afterwards: `gh auth login`, `gh auth setup-git`, your git `user.name`/`user.email`, and sign in to Claude Code.
3. **Deploys come from master only (blue/green).** blinky-server runs from `~/blinky-deploy`, a symlink to one of two worktrees of the clone, `~/blinky-deploy-a` and `~/blinky-deploy-b`. Each is detached at a master commit, with its own venv and console build. The clone `~/blinky_time` is for development; the server never runs from it.

   `blinky-deploy.timer` runs `scripts/testbox/deploy.py` every 2 minutes. It takes a new master commit only when:
   - it is the merge of a PR into master;
   - every check run on the master commit is green, including `Blinky Server Tests`;
   - the PR's last commit has a clean Claude review: the workflow's sha-stamped verdict comment, posted by `github-actions[bot]`, says "No issues found" (the `claude-review` check alone is green even when the review found issues).

   A commit still in CI waits for the next pass. A red one, or one whose review has findings, is refused. `deploy.py --accept SHA "reason"` deploys a refused commit anyway, as the owner's decision recorded in the state.

   **Only server changes restart the server.** `SERVER_PATHS` in `deploy.py` lists what the server reads or runs from its tree: `blinky-server/`, `blinky-console/`, `scripts/testbox/`, and the `corpus` submodule (with its scorers). The repo has no firmware submodule (the 2026-11-24 decoupling): its host tools are `blinky-server/hosttools/`. Slots prepared before that keep an unused `firmware/` checkout and `.arduino` links until they are cleaned by hand. A commit that passes the gate but touches none of these only moves `refs/blinky/deployable`, which the Spark training bundle is built from; nothing restarts.

   A server change is installed into the idle slot while the server keeps running. The venv is reinstalled only if `blinky-server/pyproject.toml` changed, the console rebuilt only if `blinky-console/` changed, and the slot's systemd units installed where they changed. Then the deployer stops the server, which drains (#325, `blinky_server/work/durable.py`): it refuses new work, lets every step or flash in flight finish, holds each durable job at its next step boundary, and exits. `KillMode=mixed` keeps the SIGTERM off the ffplay or UF2 copy the server is waiting for, and systemd's stop timeout is held open while it drains. Only then does the link move and the new server start. If `/api/devices` doesn't answer within 90 s, the deployer switches back and refuses the commit. `GET /api/work` shows what's in flight.

   - First install: `setup.sh` runs `deploy.py --init`.
   - State: `python3 ~/blinky-deploy/scripts/testbox/deploy.py --status` (`--dry-run` gates the latest master and changes nothing).
   - Log: `journalctl -u blinky-deploy`.
   - Never restart the server against the dev clone. To try a branch, run it on another port from `~/blinky_time`.
4. **USB speaker as the default ALSA card.** The server's player (`testing/alsa_play.py`, run by `audio_player.py`) writes to `plughw` on the default card and measures when the first sample reached the DAC (see "Audio start" above). Its ffplay fallback uses default ALSA routing (dmix), not `AUDIODEV=hw:N,0`. Pi OS Lite has no PipeWire, so without this both play to HDMI. Put the card's name (from `aplay -l`, the word after `card N:`) in `/etc/asound.conf` or the server user's `~/.asoundrc`, by name and with the `!` override (USB card numbers can change between boots, and a plain `defaults.pcm.card <name>` is rejected as "card is not a string"):
   ```
   defaults.pcm.!card Pebbles
   defaults.ctl.!card Pebbles
   ```
   Check with `ffplay -nodisp -autoexit <track>.mp3`. `BLINKY_AUDIO_PCM` in the server's environment names another PCM for the player.
5. **Enroll a chip** with its device config through `device upload` (on the box, straight to the chip, #382), and respect the 60-second rule between resets. The grid's chip takes `devices/registry/testbox_grid_v1.json` (brightness 0: see "LEDs: dark unless a test runs").

## Build service client

Dev builds come from the build service ([build-service/](../build-service/README.md), [DEV_LOOP.md](DEV_LOOP.md)). The box is a client, and its config is private and stays out of git: `~/.config/blinky/build.env` (mode 600), like `spark.env`.

```
BLINKY_BUILD_URL=http://<service address>:<port>
BLINKY_BUILD_TOKEN=<the token from 1Password>
```

The box checks every package with `blinky-server/hosttools/firmware_release.py verify-dev-package --sha SHA --dir DIR`, the same checks `download-staging` runs on a staging artifact. Unpushed work goes as a bundle against a pushed branch, from a commit, never a dirty tree: `dev.py` (see "Dev loop") makes it.

## Hardware checks

The box's hardware test has two tiers (owner, 2026-11-25):

1. **The hardware checks: simple yes/no checks that block.** Not quality checks. A PR staging → master must pass them, and a red commit must not reach the history, so they run on each PR head before it merges (`Hardware test (PR)`) and the staging runner only confirms them for the merged commit (`Hardware test (staging)`).
2. **The quality suite: scores, never blocking.** The staging suite (every edm/ track, music and beat pace) on the staging head after its checks pass.

The checks are one durable job, `test.hardware-checks` (`blinky-server/blinky_server/testing/hardware_checks.py`): the audio lock throughout, the chips lit only while it runs and dark after. Each check has a name, a yes/no and its evidence. **The phases run in order and the job stops at the first that fails**: a later phase depends on the earlier ones (no lights, and the camera can't judge anything after).

| Phase | Check | Yes when | Evidence |
|---|---|---|---|
| upload | flash | the build went through the deploy pipeline (`run_deploy`: verified package, single flash path, settings restore, post-deploy version assertion) | the version flashed |
| | version | `json info` reports the build's version | the version |
| | configured | configured as its board (device type), not safe mode | the device id |
| | stable | up 75 s (past the firmware's markStable at 60 s), no reboot since the flash | uptime, how long it was watched, `resetReason` |
| | info | `json info` has every field (`version`, `millis`, the audio counters, `fps`, `maxFrameMs`), `fps` ≥ 30, `mallocIsr` 0, its platform and LED count are the board's, `sn` is the chip's | fps, maxFrameMs, heap |
| lights | leds | `leds 40` applies (the runtime override, never saved) | the `leds` reply |
| | walk | the firmware's `testpattern` (blinky-firmware#53) lights each pixel alone while the camera watches through the saved region map (`~/.local/share/blinky-camera/regions-grid-1000us.npz`): every pixel lights exactly its own region, every region is seen, none lights for two pixels | pixels and regions seen; the dead, ambiguous, shared and unseen ones |
| audio | mic | the mic's raw level (`a.raw`) with the box speaker playing 3 s of noise is at least 4× its level in the 3 s of silence just before (medians), and the PDM is alive throughout | the two levels |
| | samples | no `audioOverruns` or `audioSamplesLost` growth while listening | the growth |
| response | one per feature | the feature responds to the stimulus (the table below) | the numbers behind it |
| interfaces | serial | 5 `json info` round trips, each answered | median round trip |
| | stream | `stream on` sends lines within 2 s; `stream off` stops them within 1 s | lines on, lines after off |
| | settings | `get` → `set` → `get` → set back → `get` of a runtime setting (`hueshift`); never saved | the values read back |
| | ble stack | the chip's `ble` report: advertising, its fleet scanner active (nRF52840) | what it reports |
| | ble scan | a host BLE scan (scan only: the server runs `--no-ble`, nothing connects or pairs) sees the chip's address | not applicable on this box: it has no Bluetooth adapter (`setup.sh`: `dtoverlay=disable-bt`) |
| | no reboot | no reset across the checks | uptime |
| | alerts | no firmware `[ASSERT]`/`[FALLBACK]` line since the flash | the lines |

Which checks a chip gets is its board's entry (`BOARDS` in `hardware_checks.py`): `testbox_grid_v1` (nRF52840, 64 LEDs, the grid's region map, the BLE stack report). The ESP32-S3's entry comes with its device config and flash path (P3). The firmware's IMU is a stub (`IMU_ENABLED` off), so there is no IMU check; the mic's power shows as the PDM's `alive` in the audio check.

**The camera here is a gate, once.** The owner made "do the LEDs work" a blocking yes/no, and only the camera can see it. The camera's timing and quality reviews stay secondary and never gate (`TEST_BOX_CAMERA_HARNESS_PLAN.md`). The lights check drives the chip over its serial connection and the camera through a worker on the system Python (`scripts/testbox/camera/camera_lights.py`, one JSON line each way: the server's venv has no picamera2).

### The response stimulus and its rules

A deterministic synthetic WAV (`testing/stimulus.py`, written once per content hash under the server's data dir; the standard library only, the server's venv has no numpy), 60 s at 22.05 kHz, sections separated by 3 s of silence:

| Section | Time | Content |
|---|---|---|
| silence | 0-2 s | |
| kick | 2-10 s | 16 kicks at 120 BPM: a sine swept 180 → 55 Hz, decay 180 ms |
| snare | 13-21 s | 16 snares at 120 BPM: noise band-passed at 3 kHz plus a 190 Hz body, decay 70 ms |
| hat | 24-30 s | 24 hats, 8ths at 120 BPM: noise high-passed at 6 kHz, decay 20 ms |
| groove | 33-57 s | 120 BPM: a kick every beat, snares on 2 and 4, hats on 8ths: long enough for the tempo and the PLP phase to lock |

The stream is captured (debug + fast, and the detection events) on the host clock through the firmware clock (`_sync_clock`). The rig's lag (ffplay's start to the sound at the mic, 0.24-0.53 s) is found as the scorer finds it: the lag where the most kick and snare hits have a detection within 40 ms (the middle of the plateau). Every rule then compares a feature **at the hits** (−20 to +120 ms) with **between the hits** (half a period later), never with silence: the box's chip detects about 3.5 onsets/s in the room's silence, so silence is no baseline. The firmware has no per-drum detector; the kick, snare and hat bands show in the spectral shape.

| Feature | Stream field | Rule (loose: is it alive and wired, not accuracy) | Measured 2026-11-25 (b211-dev), at hits vs between |
|---|---|---|---|
| onsets | TRANSIENT events | a detection at ≥ 60% of kick hits and of snare hits, and 20 points more at the hits than between them, pooled over both (a kick's tail still sounds half a beat on, and the chip detects in it: 56% and 94% between kicks in two runs) | kick 94% vs 56%, snare 100% vs 12%; pooled 97% vs 34% |
| activation | `m.nn` | ×1.3 at kick and snare hits | 0.15 vs 0.09, 0.16 vs 0.05 |
| pulse | `m.p` | +0.1 at kick and snare hits | 0.47 vs 0.31, 0.54 vs 0.15 |
| level | `a.l` | +0.1 at kick and snare hits | 0.68 vs 0.02, 0.43 vs 0.01 |
| energy | `m.e` | +0.05 at kick and snare hits | 0.58 vs 0.30, 0.49 vs 0.30 |
| flux | `m.rflux` | ×3 at kick and snare hits | 0.71 vs 0.02, 0.17 vs 0.002 |
| hfc | `m.hfc` | ×3 at snare and hat hits | 125 vs 0.09, 2.6 vs 0.04 |
| centroid | `m.cent` | at snares and at hats ≥ 2× at kicks | kicks 5, snares 31, hats 45 |
| rolloff | `m.roll` | at hats ≥ 2× at kicks, +1 | kicks 1.3, hats 37 |
| tempo | `m.bpm` | median over the groove's last 12 s within 6% of 120 BPM, or of 60 or 240 | 119.9 |
| period | `m.per` | the ACF period (62.5 Hz frames) within 8% of 120 BPM, or of 60 or 240 | 31 frames (121 BPM) |
| rhythm | `m.str`, `m.a` | strength ≥ 0.5 and active half the groove's last 12 s | 0.89, 100% |
| periodicity | `m.conf` | ≥ 0.5 over the groove's last 12 s | 0.89 |
| phase | `m.ph` | at the groove's late kicks the PLP phase sits at one value (circular concentration ≥ 0.6), and it wraps once per beat of the reported tempo (±25%) | 1.00; 2.00 wraps/s for 2.00 beats/s |
| beats | `m.q` | beat events at the reported tempo's rate (±25%) | 24 in 12 s |
| plp pulse | `m.pp` | +0.05 at the groove's late kicks | 0.26 vs 0.002 |
| density | `m.od` | within 0.5-2× the groove's detection rate | 4.2/s for 4.1/s |
| render | `v.act` | the rendered frame changes ×1.1 more at kick and snare hits (the LEDs follow) | 0.067 vs 0.047 |

The capture is kept (`hardware-checks/<job>/response.json` in the server's data dir) for a look at a failure.

### A rig error is not a firmware failure

The camera unavailable, the region map missing or not fitting (the frame brightens with every LED on but the regions don't), the speaker or ffplay failing, the build service or a package download failing (anything before the flash), a flash that never took (the chip still on its old version), the job itself crashing: the job ends with `rig_error` set and no verdict. A preflight checks the camera and the speaker before anything is flashed. The runners keep the commit `pending` "rig error: …, retrying" (never red) and try again: the next pass first, then backing off, doubling up to 30 min (`staging.rig_backoff`), so a broken rig isn't flashed every pass. A chip that stops answering or disconnects in any phase is judged a firmware failure, a "chip answers" no for that phase (a crash is the likely cause), never a rig error: the runners would otherwise retry a crashing build for good. `pr_queue.py --retest PR` and `staging.py --retest` re-queue a head, for a flake that ended in a verdict.

### Strict rules (non-blocking)

The release gate's limits that don't block yet: longest frame ≤ 100 ms, fps ≥ 60, no overrun growth, beat pace, music level and onsets, and the chip's detection latency (scored runs only; "Audio start") ([DEV_LOOP.md](DEV_LOOP.md), "Strict rules"). Every `json info` read the phases make is kept as a loop window, and the rules are judged over them at the end. They **warn** (a section in the report, "· N strict warnings" on a passing status, never red) until the owner switches one to **block** in `testing/strict_rules.py`, after the readiness tracker shows it passing on 5 builds in a row (the staging report says so and the staging runner opens one blinky_time issue). The windows that may hold one of the server's own settings saves (the deploy's restore + save, blinky-firmware#49) are excluded from the frame-time rule and listed with the save's time; nothing else is.

### Reports

One comment per head, updated in place: the hardware checks first (the verdict, each phase with its time, the table, the strict rules), then the quality section labelled non-blocking. On a PR head the quality section says the staging suite scores it after merge; on a staging head it fills in when the quality suite ends.

## Staging runner

`blinky-staging.timer` runs `scripts/testbox/staging.py` every 2 minutes from the deploy worktree. For the newest commit on blinky-firmware's `staging`:

- **No change under the firmware's paths, or the same firmware image** (fingerprint) as master's: `success` "hardware test skipped", promoted.
- **Its tree is a PR head's that passed the hardware checks** (the PR that merged it, squash-merged and up to date, so the trees are identical): `Hardware test (staging)` `success` "hardware checks passed before merge as PR #N head <sha8> (same tree)", without a run.
- **Otherwise** (a fork PR's merge, a head nobody checked): the hardware checks on it (`POST /api/test/hardware-checks`, source `staging`). A failure posts `failure`, a rig error stays `pending`.
- **On a pass** (either way) it opens the staging → master PR and turns on its auto-merge, pinned to that commit. `Hardware test (staging)` is what blinky-firmware's master ruleset requires: every staging commit gets it, and with the checks run before merge nothing red lands from a post-merge run.
- **Then the quality suite** (`POST /api/test/staging`, `blinky_server/testing/staging.py`), non-blocking: it deploys the staging build, reads 5 min of health (no reboot, every loop window from the deploy on), plays every edm/ track once for validation and the music/beat-pace checks, then checks the version, no reboot and no firmware `[ASSERT]`/`[FALLBACK]`, and judges the strict rules over all of it (release_gate's fps floor, longest frame, overrun growth, music and beat pace; the deploy's save window listed as excluded). After it, the runner reports each strict rule's readiness and opens a "Ready to enable strict rule: X" issue for a rule that is ready. It reports in the head's comment and in the score record (`staging-scores.jsonl` in the server's data dir: per commit, the version, the corpus pin, the setup, and each track's drum scores with the scorer's `lagSource`/`lagAmbiguous`), and posts `Quality suite (staging)`, **always `success`**, with a short score summary. It waits for queue mode and yields to any ready PR head (blocking checks first).

`staging.py --status` prints the state, `--retest` checks the head again, `--dry-run` prints what a pass would post without posting or submitting anything. Log: `journalctl -u blinky-staging`. The release flow around it is in blinky-firmware's [RELEASE_PROCESS.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/RELEASE_PROCESS.md).

blinky-firmware's `tools/deploy_local.py` and `tools/release_gate.py` (and `devices/test_devices.json`) live in blinky-firmware and run against a chip on the machine's own USB. On the box blinky-server owns the chip's port, so the hardware checks and the quality suite are how the box tests firmware.

## PR hardware queue

`blinky-pr-queue.timer` runs `scripts/testbox/pr_queue.py` every 2 minutes from the deploy worktree, next to the staging runner and sharing its helpers. It runs the hardware checks on each firmware PR to blinky-firmware's `staging` before it merges, and posts the commit status `Hardware test (PR)` on the PR's head (design: [DEV_LOOP.md](DEV_LOOP.md), "PR hardware queue"). Each pass takes the open PRs to `staging`:

- **Drafts** are ignored.
- **Fork PRs** (the head repo isn't `Jdubz/blinky-firmware`) are never built or tested on the box (DEV_LOOP.md, "Trust"). They get `success`, "fork PR: not hardware-tested before merge; the staging suite tests it after merge", so a required status can't block them forever. The staging runner checks their merged commit.
- **PRs with no change under the firmware's paths** (`tools/firmware_paths.json` at the head, against the merge-base with `staging`) get `success`, "no firmware change: hardware test skipped".
- **Everything else is queued:** the newest head of each PR, oldest PR first. A head superseded while it waits or runs drops out (a running test is cancelled), is marked `success` "superseded by <sha8> before its hardware test", and its successor keeps its place in the queue.

In queue mode each queued head is built on the build service by sha (a branch of the repo, so the service's trust check passes; a failed build posts `failure` and the log's tail in the PR comment), then the oldest PR's built head goes to the box (`POST /api/test/hardware-checks`, source `dev_build`): the hardware checks above, the deploy being the build's dev package through the same verified `run_deploy` path as `/api/dev/deploy` (test chips only, stored non-stable, the API key, never `X-Deploy-Tool`), refused until each chip has run 60 s since its last reset.

The result on the head: `success` "hardware checks passed: …" if every check passes, `failure` naming the first failing check, or on a rig error `pending` "rig error: …, retrying" (never red for a rig fault: retried, backing off). One comment per tested head, headed `## Hardware test (PR) — <sha>` and updated in place. Scores are not in the blocking status any more: the staging quality suite scores the merged commit.

- **Dev mode:** nothing is submitted, builds included. Each queued head gets the pending status `queued: box in dev mode since HH:MM (N waiting)`, posted only when the text changes, and the queue shows in `dev.py status`, `GET /api/box/mode` and the metrics. `dev.py mode dev --preempt` cancels running hardware checks too; their head goes back in the queue and is checked again in queue mode.
- **Sharing the box with the staging runner:** the server runs one hardware test at a time (a busy box answers 409, and the runner tries again next pass). Each runner reports its own waiting items, ready ones marked `ready`, and yields to the other's ready item that has waited longer, while the other is still reporting (a stopped runner never blocks one that runs). A head backing off after a rig error isn't reported ready.
- **Closed or merged PRs:** dropped from the queue; running checks are cancelled (`DELETE /api/test/hardware-checks/{id}`); nothing more is posted.
- `pr_queue.py --retest PR` queues the PR's head again (a flake that ended in a verdict). State: `pr_queue.py --status` (`~/.local/share/blinky-server/pr-queue.json`); log: `journalctl -u blinky-pr-queue`. Config: `build.env` ("Build service client" above); without it nothing is built and the pass says so in the log.
- **Required or not:** the owner makes `Hardware test (PR)` a required status on blinky-firmware's `staging` ruleset.

## Dev loop

A firmware change can be built, flashed to the box's test chip and checked in a few minutes, before any push or CI (design: [DEV_LOOP.md](DEV_LOOP.md)). From a blinky-firmware checkout on the box (or `--repo PATH`):

```
python3 ~/blinky-deploy/scripts/testbox/dev.py try [REF] [--hw] [--tracks a,b] [--health MIN] [--pace] [--camera] [--wip]
python3 ~/blinky-deploy/scripts/testbox/dev.py status          # mode, what waits behind it, the chips
python3 ~/blinky-deploy/scripts/testbox/dev.py mode queue      # leave dev mode: the hardware runners carry on
python3 ~/blinky-deploy/scripts/testbox/dev.py mode dev [--note TEXT] [--preempt]
python3 ~/blinky-deploy/scripts/testbox/dev.py build [REF] [--wip]
python3 ~/blinky-deploy/scripts/testbox/dev.py flash BUILD_ID
python3 ~/blinky-deploy/scripts/testbox/dev.py test info health=2 validate=techno-machine-drum pace "cmd=set odfgate 0.3"
python3 ~/blinky-deploy/scripts/testbox/dev.py test --hw [--phases upload,lights] [--expect VERSION] [--regions MAP] [--lights-brightness N]
```

- **`try` is the one command.** It switches the box to dev mode if it isn't (and says so), builds the commit on the build service, flashes it, runs the info check plus the ones asked for, and prints the version, the flash result, each check and the `json info` timers. `--hw` runs the hardware checks (above) after the flash, against the flashed version: what the PR queue will run on the head.
- **`test --hw`** runs the hardware checks on the chip as it is (no flash; `--expect` names the version it must run). `--phases` runs a prefix of them; `--regions` names another region map (a missing one shows a rig error); `--lights-brightness 0` shows the lights check failing (runtime only, `leds config` after). In `test`/`try` runs, no reboot and the end-of-run asserts (no `[ASSERT]`/`[FALLBACK]`) pass or fail; the strict rules warn; scores are reported, never gating. `dev.py status` shows each strict rule's streak. `--camera` then runs `camera_review.py` on the tracks.
- **No push needed.** An unpushed commit goes to the build service as a git bundle on its merge-base with `origin/staging` (then `origin/master`). A dirty tree is refused unless `--wip`, which builds the working tree as a throwaway commit at `refs/dev/wip` without touching your branch, index or stash.
- **Dev mode has no expiry.** While the box is in it, the hardware runners (staging and the PR queue) hold their heads with a pending status (`queued: box in dev mode since HH:MM (N waiting)`), and the wait shows in `dev.py status`, `GET /api/box/mode` and the metrics `blinky_box_mode`, `blinky_hw_queue_depth` and `blinky_hw_queue_oldest_seconds`. Switching to dev while a hardware test (hardware checks or the staging quality suite) runs waits for it; `--preempt` cancels it, and its runner tests that head again in queue mode. Switch back with `dev.py mode queue` when done.
- **Server side:** `GET`/`PUT /api/box/mode`; `POST /api/dev/hardware-checks {device_ids, phases?, expected_version?, regions?, lights_brightness?}`; `POST /api/dev/deploy {build_id, device_ids}` (the staging deploy pipeline with the build service's package, verified like a staging artifact: test chips only, stored non-stable) and `POST /api/dev/run {device_ids, checks}` (`info`, `health`, `validate`, `pace`, `commands`, in order, in one test session), both 409 outside dev mode. Each chip keeps the firmware's 60 s between resets: a dev deploy is refused (with Retry-After) until the chip has run 60 s.
- **Config:** `~/.config/blinky/build.env` ("Build service client" above); the server and `dev.py` both read it, the environment overriding it. Without it, dev deploys answer 503.

## Needed: per-server device ownership

`--no-ble` keeps the box from adopting production devices only by turning off a transport. The real requirement is that **every device belongs to exactly one server, and a server never touches a device it doesn't own**, whatever the transport or radio range. Proposed shape (to be designed here):

- **An ownership manifest per server** listing each owned device's ID, BLE MAC and USB serial.
- **Refuse what isn't listed.** No connecting to, adopting, commanding, auto-recovering or flashing a device outside the manifest; log every refusal. Unknown devices show as "foreign", read-only.
- **Check identity on connect.** `json info` `device.id` must match the manifest entry for that MAC/serial; a mismatch is a loud error.
- **One owner per device.** A check fails if a device is in both manifests, or a production device is in blinky-firmware's `devices/test_devices.json`.
- **Enrolling a chip** is an explicit, logged step, never something discovery does on its own.

BLE testing in the box waits on this.

## Open questions

- Should the box test BLE (fleet commands, BLE DFU)? It first needs per-server device ownership. Today the hardware checks read the chip's own BLE report and would scan for it (scan only) if the box had a Bluetooth adapter; it has none.
- Should the server trigger SWD recovery itself when USB recovery fails (blinky-firmware's `BOOTLOADER_WORKFLOW.md`)?

## Ideas (not decided)

Written before the box was built, without an owner request. None of this is a requirement; take any of it up only with the owner.

- **Trusting the box's numbers.** Checks that would let the staging suite gate on validation numbers: host timing (spread of the estimated audio latency over 20 plays of one track, no throttling, logged with `vcgencmd`); repeatability (same firmware and track, 5 runs, small onset-score spread); agreement with earlier bench results for the same build.
- **Acoustic isolation.** An enclosure would make runs independent of room noise. Size, materials and construction are open.
- **Speaker bass.** The Pebbles is small and ported; a sweep would show how well it reproduces kick fundamentals (~50 Hz).
- **Calibration gear.** A measurement mic and an SPL meter to set and record the playback level at the chip.
- **Grid data line.** A series resistor on the data line and a bulk capacitor at the grid's 5 V input are common WS2812 practice; a level shifter if flicker ever appears.
