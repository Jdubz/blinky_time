# Acoustic test box: build plan

**Status:** planned (2026-10-20; still nothing built as of 2026-10-21). This doc covers the hardware, the enclosure, and the gates the box must pass before its numbers are trusted.

## Why

1. **Repeatable runs.** Validation F1 currently depends on the room: background noise, where the speaker sits, what else is playing. A closed, calibrated box makes runs comparable across days and firmware builds.
2. **Unattended testing.** The box is self-contained: a host, a speaker, a camera and the test chips are wired permanently. Release gates (`tools/release_gate.py`) and validation suites can run without anyone setting up the bench.
3. **Safe recovery.** Every chip in the box has SWD wired, so a bad candidate never needs the box opened.
4. **Testing stays off the production machine.** blinkyhost is production (see below). Candidates, `--dev` builds and validation runs move to the box, so nothing unreleased runs on the machine that manages finished devices.

## Two hosts, two roles

| | blinkyhost (production) | Test box (this plan) |
|---|---|---|
| Runs | lemon-cart (sibling repo): WiFi AP, Caddy, Hub UI, station-api, blinky-server | blinky-server only, including its Spark client (#288): it submits GPU jobs and receives the Spark's webhooks. Large assets stay in the Spark's object store; the box keeps only data it collects and test files. |
| Manages | Finished devices in the fleet | The box's own test chips |
| Firmware | Latest **stable** only, via `./scripts/deploy.sh` | Candidates and `--dev` builds, test chips only |
| Audio | Station audio (PipeWire) for guests | Box speaker, validation corpus only |
| Validation runs | None | All of them |

Rules that follow from this:

- **The two servers never share devices.** A finished device is never plugged into the box. A box chip is never enrolled on blinkyhost.
- **No BLE on the box server at first.** Start it with `--no-ble`. blinky-server has no discovery allowlist, so a box server with BLE on could adopt a production device in radio range, and its auto-recovery could flash it. USB-only discovery makes that impossible. BLE testing in the box waits for a discovery allowlist (see Open questions).
- **No validation on blinkyhost.** A validation run plays music through the host's audio output, which on blinkyhost is the station's sound system.
- **`deploy.sh` stays production-only.** It targets `blinkyhost.local:8420` (hardcoded) and ships only stable releases. The box flashes its chips with `tools/deploy_local.py` and `tools/release_gate.py`, run on the box's Pi 5.

## Architecture

```
            Ethernet (cable gland)
                    |
  +-----------------+------------------------------------------------+
  |  Pi 5 (blinky-server)                                            |
  |   - USB (own port) ---------------> USB speaker (own baffle)     |
  |   - CSI camera  ------------------> views every chip's LEDs      |
  |   - USB ---> powered hub, per-port switching (uhubctl)           |
  |                 |-- test chip A (+ LEDs)                         |
  |                 |-- test chip B (+ LEDs)                         |
  |                 |-- test chip C (+ LEDs)                         |
  |                 '-- Pi Zero W (USB gadget or power only)         |
  |                                                                  |
  |  Pi Zero W (SWD recovery) --- SWDIO/SWCLK/RESET ---> each chip   |
  +------------------------------------------------------------------+
```

### Host: Pi 5, not the Pi Zero W

The Pi Zero W cannot host blinky-server reliably:

- **Single core.** A validation run plays the track (`ffplay`, `blinky_server/testing/audio_player.py`) while reading several serial streams and timestamping every detection. On one 1 GHz ARM11 core, scheduling delay goes straight into the timestamps. Scoring uses a 100 ms onset window (`scoring.py: ONSET_TOLERANCE_SEC`) and a per-run latency estimate. Jitter that changes with load does not error; it quietly makes F1 vary.
- **Playback start time is taken when ffplay is spawned**, not when sound leaves the speaker. The spawn-to-sound delay has to be small and stable, which a loaded single-core host does not guarantee.
- **ARMv6.** Prebuilt packages for `pydantic-core` and `uvicorn[standard]` extras are poor or missing, and building them in 512 MB is impractical.
- **I/O.** One micro-USB OTG port, no analog audio, and WiFi and BLE share one radio.

The Pi 5 has four cores, USB 3 on two controllers, Gigabit Ethernet, and the same 22-pin narrow camera connector as the Zero, so the existing camera cable fits.

### The Pi Zero W keeps its SWD job

SWD recovery is light work, and the setup is already proven (`swd-flash.local`). Put the Zero inside the box and wire SWD to every chip there, following the wiring in CLAUDE.md (SWDIO=BCM 24, SWCLK=BCM 25, RESET=BCM 18 for the first chip).

**Several chips on one Zero.** OpenOCD's `bcm2835gpio` driver takes its pins from config. Give each chip its own GPIO set (SWDIO/SWCLK/RESET), and have `recover.sh` take a `--chip` argument that selects the set. The Zero has enough free GPIO for 4–5 chips. This needs a change in `~/swd-recovery` and a per-chip pin table on the Pi. Keep the reset-line crosstalk fix: hold the chip's RESET pin high during long writes, then release it to input.

### Needed: per-server device ownership

`--no-ble` is a stopgap: it keeps the box from adopting production devices only by turning off a transport. The real requirement is that **every device belongs to exactly one server, and a server never touches a device it doesn't own**, whatever the transport or radio range. Proposed shape (to be designed in its own issue):

- **An ownership manifest per server.** It lists each owned device's ID, BLE MAC and USB serial. blinkyhost gets the production manifest; the box gets the test manifest.
- **Refuse what isn't listed.** The server doesn't connect to, adopt, command, auto-recover or flash a device outside its manifest, and logs every refusal. Unknown devices are listed as "foreign", read-only.
- **Check identity on connect.** The device's `json info` `device.id` must match the manifest entry for that MAC/serial. A mismatch is a loud error, not a reassignment.
- **One owner per device.** A check (CI or a script) fails if any device appears in both manifests, or if a production-manifest device appears in `devices/test_devices.json`.
- **Enrolling a new chip** is an explicit, logged step that adds it to one manifest, never something discovery does on its own.

This also replaces the current "a test device is whatever is listed in `test_devices.json`" with a single source for who owns which device and which firmware channel it may take.

## Parts

| Part | Notes |
|------|-------|
| Raspberry Pi 5, 4–8 GB | Official 27 W PSU. Active cooler. |
| USB speaker | No audio HAT: playback goes to a USB audio device. In use: JBL Pebbles (USB-powered, USB audio, ALSA card name `Pebbles`); small and ported, so check its bass against the ~50 Hz requirement in the first REW sweep before trusting kick results. Plug it into the Pi directly, on the other USB controller from the hub (the Pi 5 has two; each serves one blue and one black port), never into the switched hub: a `uhubctl` cycle or hub reset would cut the speaker mid-run. Sealed, not ported (port noise is an artifact). Must play down to ~50 Hz, or kick tests mean little. |
| Camera | The existing Zero camera, now on the Pi 5's CSI port: Camera Module v1 class, **OmniVision OV5647**, 5 MP, rolling shutter, fixed focus. Measured on the Pi 5 (2026-10-27): 640×480 full-view binned mode, at most 67.5 fps (14.8 ms frames, spacing sd ≈ 1 µs). How it joins the harness: `docs/TEST_BOX_CAMERA_HARNESS_PLAN.md`. A global-shutter camera stays the upgrade if that plan's reference-LED timing check fails. |
| Reference LED | One or two LEDs driven from Pi 5 GPIO, in the camera's view, for camera timing calibration (`TEST_BOX_CAMERA_HARNESS_PLAN.md`, Calibration). |
| Powered USB hub with per-port power switching | Pick from the `uhubctl` supported list and confirm per-port switching (not ganged) with `uhubctl` **before** building it in. Recovery depends on cycling one port. |
| Pi Zero W | SWD recovery (above). |
| Test chips | XIAO nRF52840 Sense, each driving a string of LEDs in the camera's view (the final design; the camera harness judges the visuals from these strings). Keep each string apart from the others in the image. |
| LED power | Separate 5 V supply for LEDs with a common ground. Keep brightness low: heat and supply noise. |
| Measurement mic | miniDSP UMIK-1 or UMIK-2, for calibration only. |
| SPL meter | For setting playback level at the chips' position. |
| Enclosure materials | 18 mm MDF, Green Glue, Sorbothane pads, mineral wool (Rockwool or OC 703-class boards), acoustically transparent fabric, compression gasket, draw latches, cable glands, acoustic putty, matte black paint. |

## Enclosure construction

### Size and shape

- **Interior ~35 × 49 × 66 cm** (ratio 1 : 1.4 : 1.9, which spreads room resonances apart). Never a cube.
- If the build allows, angle one pair of walls 5–10° away from parallel, or make the lid sloped.
- First resonance is about c/2L ≈ 260 Hz on the long axis. Kick fundamentals (50–100 Hz) sit below it, where the box acts as a uniform pressure chamber; that is fine. The trouble zone is roughly 260 Hz–2 kHz, where snare body lives.

### Isolation: mass, decoupling, airtightness

- **Box in a box.** The outer shell is two layers of 18 mm MDF with Green Glue between them. The inner box sits on Sorbothane pads with a 2–5 cm gap to the outer shell, filled with mineral wool.
- **Sealing matters more than wall thickness.** Put a continuous compression gasket around the lid and pull it down with draw latches. Bring every cable through a cable gland or a lined S-shaped channel, then seal the gaps with acoustic putty. One open hole undoes the rest.
- **Check for flanking paths:** rigid feet, cables pulled tight between shells, screws that pass through both walls. Nothing rigid may connect the inner box to the outer one.

### Absorption

- Line every inner surface with 5–10 cm of mineral wool, and put denser wool in the corners.
- Cover all wool with fabric. Loose fibers must not reach the boards.
- Skip thin foam: per centimeter, it absorbs far less than mineral wool.
- **Be realistic about the low mids.** Porous absorbers work well only when they are about a quarter wavelength thick, which is ~28 cm at 300 Hz. The box will not be anechoic. The goal is a **stable, measured** response, not a perfect one (see Calibration).

### Vibration

The PDM MEMS mics respond to structure-borne vibration as well as to sound.

- Mount the speaker on its own baffle, isolated with Sorbothane, not on the shelf the chips sit on.
- Mount the chips on a separate shelf on Sorbothane pads. Sound must reach the mics through the air, not earlier or additionally through the frame.
- Secure all cables so they cannot rattle at loud playback.

### Layout inside

- Put the chips on an arc at equal distance from the speaker so they all get the same SPL.
- Keep the chips away from walls and corners, where pressure builds up.
- Fix every chip in a jig or 3D-printed holder so positions are identical across runs. Mark the measurement-mic position the same way.
- Point the chips' mic ports at the speaker the same way, and match their enclosures to the fleet's. Drilled mic ports change amplitude by ~3×.
- Paint the inside around the LEDs matte black so the camera sees no reflections. With the lid closed, the only light is the LEDs, so the lighting is constant.
- Mount the camera rigidly so every LED is in frame. Lock its exposure, gain and white balance; auto-exposure hides brightness changes.

### Heat

A sealed, lined box with a Pi 5, a powered speaker and LEDs will get hot.

- Bring air in and out through two lined S-shaped ducts.
- Put a slow, quiet fan at the outlet duct, not inside the box.
- Keep the fan at a fixed speed during runs (or off for short runs), so its noise is the same in every run.
- Log the Pi's temperature (`vcgencmd measure_temp`) and throttling state (`vcgencmd get_throttled`) with each run. A throttled host is a test failure, not a slow run.

### Power and wiring

- Bring mains in through one panel inlet. The Pi PSU, hub supply and LED supply all live inside.
- Ethernet comes in through a gland. Do not rely on WiFi inside a dense box.
- Label every USB port and SWD harness with the chip's device ID.

## Software setup

1. Flash Raspberry Pi OS Lite 64-bit with Raspberry Pi Imager (hostname `blinkytest`, user `jdubz`). `setup.sh` gives that user passwordless sudo for everything: the box is a dedicated dev/test machine, and the Claude session that develops on it administers it (packages, services, scripts) without a password prompt. Then copy `scripts/testbox/` to the card's boot partition as `blinky-testbox/`, add your SSH key under `user:` → `ssh_authorized_keys`, and append `- [ bash, /boot/firmware/blinky-testbox/setup.sh, --user, jdubz ]` to `runcmd` in `user-data`. On first boot cloud-init runs `setup.sh` (log: `/var/log/blinky-testbox-setup.log`; result: `/var/lib/blinky-testbox/setup-ok` or `setup-failed`). The script can be re-run at any time: `sudo ~/blinky_time/scripts/testbox/setup.sh`.
2. `setup.sh` installs the packages (Python, `ffmpeg` for `ffplay`, `uhubctl`, `udisks2`, `gh`, Node), clones the repo (so the validation corpus `blinky-test-player/music/edm/` comes with it), installs blinky-server with the `firmware` and `ble` extras (the server imports bleak at startup, so it can't run without `ble`; BLE stays off through `--no-ble` and the OS-level overlay below, see "Two hosts, two roles") and builds blinky-console, all in the deploy worktree (step 3). It also installs the passwordless sudo grant (`scripts/testbox/sudoers.in`), turns Bluetooth off at the OS level (`dtoverlay=disable-bt`), and starts `blinky-server.service` with `--no-ble`. Then the development tooling, so the box runs every script and CI check itself with the same pins as CI: arduino-cli and the firmware toolchain (`scripts/setup_toolchain.sh`), cppcheck 2.16.0 built as CI builds it, GCC 13 as `$CXX` (CI's host compiler; GCC 14 rejects TFLite's bundled flatbuffers), adafruit-nrfutil, blinky-server's `dev` extra (ruff, mypy, pytest), a dev venv `~/.venvs/blinky` (system packages plus tflite, librosa, mir_eval), blinky-serial-mcp registered with Claude Code against the box's own server, the git hooks, and Claude Code itself. `ml-training/requirements.txt` is not installed: training runs on the Spark, and its `numpy<2.0` pin has no build for the box's Python 3.13. The server creates its own API key in `~/.blinky-api-key` on first use. Manual afterwards: `gh auth login`, `gh auth setup-git`, your git `user.name`/`user.email`, sign in to Claude Code (run `claude` once), and add the Pi's `~/.ssh/id_ed25519.pub` to `swd-flash`.
3. **Deploys come from master only.** blinky-server runs from `~/blinky-deploy`, a symlink to one of two worktrees of the clone, `~/blinky-deploy-a` and `~/blinky-deploy-b`. Each worktree is detached at a master commit and has its own venv and console build. The clone itself, `~/blinky_time`, is for development, and the server never runs from it.

   `blinky-deploy.timer` runs `scripts/testbox/deploy.py` every 2 minutes. The deployer takes a new master commit only when:
   - it is the merge of a PR into master;
   - every check run on the master commit is green, including `Blinky Server Tests`;
   - the PR's last commit has a clean Claude review. This means the workflow's sha-stamped verdict comment, posted by `github-actions[bot]`, says "No issues found". The `claude-review` check alone is green even when the review found issues.

   A commit still in CI waits for the next pass. A red one, or one whose review has findings, is refused. `deploy.py --accept SHA "reason"` deploys a refused commit anyway, as an owner's decision recorded in the state.

   **Only server changes restart the server.** The server's paths are the ones it reads or runs from its tree:
   - `blinky-server/`;
   - `blinky-console/`;
   - `tools/` (it runs `firmware_release.py`, `uf2_upload.py` and `usb_reset.py`);
   - `devices/`;
   - `blinky-test-player/music/`;
   - `scripts/testbox/`.

   A commit that passes the gate but changes none of these since the commit the server runs only moves `refs/blinky/deployable`, and nothing restarts. That covers firmware, ml-training, docs and CI. The Spark training bundle is built from that ref, so ml-training changes reach training once their CI and review pass, without a restart.

   A server change is installed into the other slot while the server keeps running untouched. The venv is reinstalled only if `blinky-server/pyproject.toml` changed, and the console rebuilt only if `blinky-console/` changed. Right before the restart, after that slow part, the deployer checks whether the server is flashing, deploying firmware or running a test job. A restart mid-flash can leave a device in its bootloader, so a busy server leaves the prepared slot for the next pass.

   The deployer then swaps the symlink and restarts the server. If `/api/devices` doesn't answer within 90 s, it swaps back, restarts, and refuses the commit.

   The deployer runs from the live slot, so it is merged code too. So is the Spark training bundle, which blinky-server builds from its own checkout's HEAD. Both slots share the clone's `.arduino` toolchain through a symlink, for server-side compiles.

   - First install: `setup.sh` runs `deploy.py --init`.
   - State: `python3 ~/blinky-deploy/scripts/testbox/deploy.py --status` (`--dry-run` gates the latest master and changes nothing).
   - Log: `journalctl -u blinky-deploy`.
   - Never restart the server against the dev clone. To run a branch, run it on another port from `~/blinky_time`.
4. Make the USB speaker the default ALSA device. `audio_player.py` deliberately uses default ALSA routing (dmix), not `AUDIODEV=hw:N,0`, and Pi OS Lite has no PipeWire, so without this ffplay plays to HDMI. Find the card's name with `aplay -l` (the word after `card N:`) and put it in `/etc/asound.conf` (or the server user's `~/.asoundrc`) by name, with the `!` override (`card` is numeric in the system config, so a plain `defaults.pcm.card <name>` is rejected as "card is not a string"), not number, since USB card numbers can change between boots:
   ```
   defaults.pcm.!card <name>
   defaults.ctl.!card <name>
   ```
   Check with `ffplay -nodisp -autoexit <track>.mp3`. USB audio adds its own buffering delay; the host-timing gate (Acceptance gates) is what shows it is stable.
5. **Test devices.** Only devices listed in `devices/test_devices.json` may take candidates and `--dev` builds. Adding box chips to that list is a deliberate decision made in its own reviewed change. Chips not on the list get stable releases only, like the rest of the fleet. Today the list holds `hat_v1`, which is a finished device. Once the box chips pass the gates, take `hat_v1` off the list in the same kind of reviewed change, so every finished device takes stable releases only.
6. Enroll the chips with the standard workflow, and respect the 60-second rule between resets during setup.

## Calibration

Repeat this whenever anything inside the box moves or changes.

1. Place the measurement mic at the marked chip position. Run REW sweeps from 20 Hz to 20 kHz with the lid closed. Save the response curve, labeled with the date.
2. Set playback level with the SPL meter at the chip position. Pick one reference level and record the ALSA mixer setting that produces it.
3. Measure the background noise with the lid closed and the speaker silent, with the room quiet and then with music in the room.
4. If the response has peaks of more than ~10 dB, decide whether to EQ the playback. Record the decision either way.

## Acceptance gates

The box's numbers are not trusted until all of these pass. The thresholds are proposals; set the final values from the first measurements.

| Gate | Pass condition |
|------|----------------|
| Isolation | At least 30 dB less room noise inside than outside, from 500 Hz to 4 kHz. Report bass isolation separately. |
| Host timing | Over 20 plays of the same track, the spread of the estimated audio latency is under 5 ms, with no throttling. |
| Repeatability | Same firmware, same track, 5 runs: onset F1 spread ≤ 0.02 per chip. |
| Agreement | Box F1 on the `edm/` corpus for the current stable firmware is within a stated margin of the last recorded bench results for that build (`test-results/`). Compare against the records; don't run validation on blinkyhost. Explain any difference before relying on the box. |
| Isolation from production | With blinkyhost's fleet powered on nearby, the box server's device list shows only box chips. |
| Recovery | Every chip: a forced bad flash is recovered by `uhubctl` cycling, then by SWD from the Zero, without opening the box. |
| Camera | Every LED is visible in one locked-exposure frame, with no reflections, and the camera harness's calibration and gates pass (`TEST_BOX_CAMERA_HARNESS_PLAN.md`). |

## Rollout

1. **Open-bench prototype.** Pi 5 + USB speaker + one chip on an open table. Run the validation suite and pass the host-timing gate before building any enclosure.
2. **Build the enclosure** and the internal mounts.
3. **Calibrate** and pass the acceptance gates.
4. **Move the release gate** to the box: `release_gate.py` runs on the box's Pi 5 against a box test chip. Today the gate talks to the chip's serial port directly (through `deploy_local.py`), while on the box blinky-server owns that port. Before this step, either the gate stops the box server for the run, or it gets a mode that goes through the box server's API. The gate's missing timing checks (#258; today it only has a 60 fps disaster floor) are a natural fit for the box's camera (`TEST_BOX_CAMERA_HARNESS_PLAN.md`, Rollout step 5).
5. **Retire test duty from blinkyhost:** take `hat_v1` off `devices/test_devices.json` (Software setup, step 4).

## Open questions

- How many chips, and how long should each LED string be?
- Multi-chip SWD: per-chip GPIO sets on one Zero (planned above), or a physical selector switch?
- Should the box also test BLE (fleet commands, BLE DFU)? That first needs per-server device ownership (see above), so the box server can never adopt a production device. Also, a shielded box changes the radio environment compared with real installs.
