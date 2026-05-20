# Open Issues — 2026-05-20

Successor to `docs/archive/OPEN_ISSUES_2026_05_19.md` (§8 doc-succession).
That marathon session closed nearly everything in code; this doc carries
forward **only what remains**, which is almost entirely **production
deployment** plus a couple of deferred validations. The archived doc
retains the full per-item history, root-cause findings, and the
won't-do rationales — read it for the "why" behind anything below.

## Status of the 2026-05-19/20 work (all code shipped to `staging`)

| Area | State |
|---|---|
| §1.1 gossip-ACK re-broadcast | code shipped; **bench-validated 20/20**; full-fleet-at-range validation rides on the rollout below |
| §1.2 scene→generator | shipped (server + console + Hub) |
| §1.3 flash cascade bug | fixed (PR #145) |
| §1.4 big_bucket button debounce | firmware shipped; **pending flash to big_bucket** |
| §2 PR #144 review items | all closed (incl. RxSlot packing) |
| §3.1 BL app-handshake watchdog | shipped — BL `0.8.0-10-g4c5faae`, GPREGRET-based, bench-validated |
| §3.2 / §3.3 / §3.4 BL recovery | shipped |
| §4.1–4.4 service hardening | shipped to lemon-cart (canary probes, sd_notify, MemoryMax, boot-verify); **pending `install.sh` deploy** |
| §5 Hub UI (generator row + hue sliders) | shipped to lemon-cart; **pending `install.sh` deploy + `npm run build`** |
| §6 extended-adv scanning | **WON'T-DO** — ext-adv only on hci1 (the audio controller); see archive for the full hardware-wall analysis |

---

## 0. ACTIVE (2026-05-20 PM): SWD reset-line crosstalk fix + 06ACEB recovery (IN PROGRESS)

**Root cause found — high value, affects all SWD recovery.** SWD flashing via
the `swd-flash.local` Pi was failing on the SoftDevice write with
`Wrong parity detected` / `Error waiting NVMC_READY` /
`clearing lockup after double fault` / **`external reset detected`**. It is
**NOT** power, **NOT** the rig, **NOT** my config changes, and **NOT** a faulty
chip. The cause: the **reset line (BCM 18 / physical pin 12), left as a floating
high-impedance input** (held high only by the chip's weak internal pull-up),
picks up **crosstalk from the adjacent fast-toggling SWDIO/SWCLK** during the
long ~440 KB algorithm-driven SoftDevice write and **glitches the chip into
reset mid-flash**. Brief ops (connect, examine, MBR write, CTRL-AP mass-erase)
always survived; only the long SD write didn't — and it failed at a *different*
sector each time (the signature of a margin problem, not a bad cell).

**Fix (PROVEN this session):** drive BCM 18 firmly HIGH during the SWD flash so
it can't glitch low — on a *different* line than SWD, so no conflict:
```
sudo timeout <n> gpioset -c gpiochip0 18=1 &   # hold reset deasserted
sleep 1; sudo ./recover.sh                      # MBR+SD+BL all Verified OK
```
With BCM 18 held high the full restore **programmed and Verified OK**.
(`srst` is NOT usable on nRF52: a pin reset also resets the debug AP, so
`reset_config srst_only` breaks `reset halt` with `AP write error`.)

**TODO — bake in next session:**
- [ ] Add the `gpioset 18=1` hold into `~/swd-recovery/recover.sh` (Pi) so SWD
      restores are reliable by default.
- [ ] **Gotcha to handle:** a `timeout`-killed `gpioset` leaves BCM 18 as a
      *stale output*. If left output-LOW it HOLDS THE CHIP IN RESET (no USB
      enumeration). Always release to input afterward (`gpioget -c gpiochip0 18`).

**06ACEB recovery status (resume next session):**
- Was on ancient stock **BL 0.6.1** + **corrupt LittleFS** (InternalFS
  0xED000–0xF3FFF) → crash-looped every ~3 s *before* `RebootFrequencyCounter`
  self-heal could run (the documented gap, `RebootFrequencyCounter.h:32-34`).
- CTRL-AP mass-erased; **MBR + S140 7.3.0 + BL 0.8.0-4 restored via SWD with
  BCM 18 held high — Verified OK**; b190 firmware UF2 then dropped.
- **Last observed: not enumerating on USB** — most likely the `gpioset`
  leftover held BCM 18 low (reset) during/after the b190 write. BCM 18 has
  since been released to input. **Next:** power-cycle → check if b190 boots to
  safeMode; if not, re-drop b190 (BL is good now) → `deploy-bootloader.sh` to
  update BL 0.8.0-4 → 0.8.0-10 → push device config → verify. Chip is
  SWD-accessible with a known-good BL, fully recoverable.
- **Caution:** blinky-server + lemoncart-canary were stopped during recovery and
  **restarted at end of session**. If 06ACEB re-enumerates as DFU, server
  auto-recovery could flash it — verify its state before trusting auto-actions.
- **Identity note (don't confuse the two):** the unconfigured chip the server
  auto-recovers over BLE-DFU as `Blinky-4A33` / `FA:E6:7D:A9:8B:3A` is the
  **bench test chip `659C8DD3ADF84A33`** (name = `Blinky-<DEVICEID[0] low16>` =
  `4A33`), NOT 06ACEB (`…8CB7`). 06ACEB advertises/USB-enumerates separately;
  as of this writing it is **absent from the fleet** (still not enumerating).
  When checking "is auto-recovery touching 06ACEB?", match on the BLE address /
  `Blinky-8CB7`, not on "the unconfigured DFU device."
- Lesson reinforced: don't attempt a *surgical partial* SWD erase on a
  crash-looping chip (its firmware re-arms the hardware WDT every boot,
  `CONFIG.HALT=1`, which resets a halted core mid-erase) — mass-erase + full
  restore is the correct, simple path.

---

## 1. Production rollout (the main remaining lever)

Everything above is on `staging` / committed to the lemon-cart repo but
**not yet on the production fleet or carts.** This is a careful,
authorize-each-step operation — sealed devices recover only via
BLE-DFU, and the session's brick incident is the reason for the caution
([[feedback-flash-safety-policy]], [[feedback-no-unauthorized-retries]],
[[feedback-test-chip-first]]).

### PROGRESS (2026-05-20 PM): §1b USB pass DONE for the connected devices

The "one last USB flash" (BL→0.8.0-10 + firmware b190) is **validated on
real hardware and complete** for every device that was on USB this session:

| Device | Was | Now |
|---|---|---|
| `tube_v2` (75F18FDAF3360545) | b179 / **BL 0.8.0-4** | b190 / BL 0.8.0-10, configured, 60 LEDs |
| `cart_inner` (062CBD12EB6961C8) | b179 / BL 0.8.0-7 | b190 / BL 0.8.0-10, configured, 104 LEDs |
| `cart_outer` (D4E1CB84C852EE41) | b179 / BL 0.8.0-7 | b190 / BL 0.8.0-10, configured, 96 LEDs |
| `scarf` (D63BD31605B36A15) | b162 / **BL 0.8.0-4** | b190 / BL 0.8.0-10, newly configured (see [[project-scarf]]) |
| `sourpuss_hat` (659C8DD3ADF84A33) | b190 already | b190, newly configured (see [[project-sourpuss-hat]]) |

**Confirmed on real hardware:** the §1b sequence — `deploy-bootloader.sh`
BL→0.8.0-10, then re-drop the b190 app — works from **both** starting BLs
(0.8.0-4 and 0.8.0-7). Crucially, **BL 0.8.0-4's UF2 silent-fail is real
and observed**: a plain app UF2 on a 0.8.0-4 device sticks in DFU, but the
BL self-update to 0.8.0-10 (via MBR) succeeds and fixes it. Done **surgically**
(deploy-bootloader.sh + `uf2_upload.py --already-in-bootloader` when already
in DFU), one device at a time, **no fleet broadcast** — deliberately avoiding
deploy.sh whose post-flash `restore_runtime_settings`/`save` would reset
runtime tunables on the whole present fleet.

**Operational notes for the remaining devices:**
- `deploy-bootloader.sh` reports a **false-negative exit 4** ("did not return
  to APP mode" / "cannot re-read BL version") on the headless already-in-DFU
  path — it unmounts the temp drive after the copy, so its re-read finds
  nothing. The BL update still succeeds; confirm by reading the UF2 drive's
  `INFO_UF2.TXT` banner during the subsequent app flash. Worth fixing.
- `make uf2-upload` regenerates `types/Version.h` via the Makefile `version`
  target, which drops the git-SHA `build.sh` embeds (device then reports plain
  `b190`). `git checkout` that file; don't commit it.

**STILL ON b179 (remaining §1a/§1b targets):** `big_bucket`, `cart_umbrella`,
and the 3 `long_tube`s — all sealed / BLE-only, not connected this session.
They need either a USB pass (open each once) or the BLE-DFU path.

### Deploy-tooling audit (2026-05-20) — done before trusting the rollout

All on the bench chip `659C8DD3ADF84A33` (the one SWD-recoverable device;
forced BLE-only by cutting USB while the swd-flash Pi held it on 3V3).

**Fixed + validated (committed):**
- `protocol.py`: `save`/`load` got a 10 s serial timeout (was 2 s →
  every deploy false-failed at the post-flash `save`).
- `deploy.sh` post-deploy assertion: scoped to `--devices` targets,
  version prefix-match (`b190` vs `b190-<sha>`), BLE-aware state
  (`present` is healthy), heredoc to kill bash-expansion noise. Full
  happy path now exits 0 on the serial bench path (first time ever —
  the save-timeout had always aborted before reaching it).
- `deploy-bootloader.sh`: headless-compatible (explicit by-label mount,
  was assuming desktop udisks auto-mount) + stale-mountpoint
  false-positive fix. Validated end-to-end on blinkyhost.
- **SHOWSTOPPER — canary killed BLE-DFU flashes.** The orchestrator
  pauses the broadcaster for a whole flash; the canary saw
  `broadcaster.running=false`, and after 90 s restarted blinky-server,
  aborting the transfer (device stuck in BLE-DFU recovery). Fixed:
  `/api/fleet/status` now exposes `active_flashes`; the canary treats a
  paused broadcaster during a flash as healthy. **Validated: a full
  ~6.5-min BLE-DFU flash ran to `flash=ok` with blinky-server
  NRestarts=0.**

**Remaining gaps (decide before/at rollout):**
- **deploy.sh post-flash BLE verification — FIXED (commit `4464d94f`).** The
  earlier gap (deploy.sh false-failed at the restore/verify step for sealed
  BLE-`present` devices) is closed: `run_fleet_command` (restore/save) now
  accepts the server's `skipped:` response for non-connected devices (the
  fleet broadcaster still delivers), and the post-deploy assertion accepts a
  `present` BLE device without `json info` (its version was already verified
  by the flash job, which deploy.sh gates on). Connected (serial/GATT) devices
  still get the full version/fps/overrun re-check. **Outstanding: one clean
  end-to-end all-BLE happy-path `deploy.sh` run to exit 0** — the code fix is
  bench-validated on the serial path; the all-BLE happy path hasn't been run to
  exit-0 yet (the failure-injection runs in §2 exercised the flash, not a clean
  deploy.sh pass).
- **BLE-DFU is slow (~6.5 min, MTU negotiated 23 / chunk 20).** The
  §3.3 MTU-247 bump did NOT take effect on this BLE-DFU connection
  despite BL 0.8.0-10. Worth investigating (the BL DFU service may not
  request the larger MTU, or the host capped it) — a fleet rollout at
  20-byte chunks is ~12× slower than intended.
- **Deployed canary unit still has `ProtectHome=yes`** (pipewire probe
  fails — alert-only). The `read-only` unit fix is committed to
  lemon-cart but not yet `install.sh`-deployed.

### 1a. Fleet firmware: b179 → b190
Picks up §3.1 firmware clear, §3.4 fresh-build reformat, §1.4 button
debounce, gossip-ACK firmware bits, RxSlot packing. Sealed devices →
BLE-DFU via `deploy.sh`. The BLE-DFU FLASH path is validated end-to-end
on the bench (with the canary fix). One device at a time; test chip
first; ≥75 s uptime between resets (60-second rule). The post-flash BLE
false-fail is fixed (`4464d94f`): deploy.sh now treats a `present` BLE
device as healthy and accepts `skipped:` on the broadcast restore/save,
so a clean BLE flash should exit 0 — `flash=ok` in the job result
remains the authoritative version-verify either way. Validate
big_bucket's phantom presses stop after its flash (closes §1.4).

### 1b. Fleet bootloader: 0.8.0-7 → 0.8.0-10 — via "one last USB flash"
**Strategy (operator decision 2026-05-20):** open each device for a
single USB pass, flash BL → 0.8.0-10 **and** firmware → b190 together,
then seal and run **BLE-only forever**. `deploy-bootloader.sh` is
USB/UF2-only (no BLE path) so the BL leg *must* happen over USB; doing
it once-and-for-all while we have the devices open avoids ever needing
BL-over-BLE on a sealed device.

**Why 0.8.0-10 is the right "forever" BL — it guarantees re-attemptable
firmware-over-BLE delivery.** No single BLE transfer can be guaranteed
(the link can drop), but 0.8.0-10 guarantees a botched firmware-over-BLE
attempt never strands a sealed device unrecoverable:
- **Dual-transport BLE-DFU** (0.8.0-4+): the BL always exposes the
  BLE-DFU service, so the server can (re-)push firmware over BLE.
- **DEFAULT_TO_OTA_DFU**: an invalid-CRC app slot auto-enters BLE-DFU →
  server re-pushes.
- **§3.1 GPREGRET app-handshake watchdog** (NEW in 0.8.0-10): a
  *crashy-but-valid* app that never reaches `begin()` is forced into
  DFU after 3 strikes → BLE-DFU → re-push. This is the gap that cost a
  controller 2026-05-19 and the reason 0.8.0-10 (not 0.8.0-7) is the
  forever BL.
The BLE-DFU app-flash path itself is validated end-to-end on the bench
(see audit above: full ~6.5-min flash to `flash=ok`).

deploy-bootloader.sh is validated on the bench (headless fix).

**Sequencing per device (USB pass):** BL via `deploy-bootloader.sh`
first, then firmware via `deploy.sh`, ≥75 s uptime between resets
(60-second rule), confirm app boots clean before sealing.

### 1c. lemon-cart deploy (`install.sh`)
Deploys §4 (canary `pipewire-sinks`/`bluetooth-rfkill` probes +
`boot-verify` timer) and §5 (Hub UI: generator row, hue sliders,
scenes section removed). `install.sh` runs the modules idempotently;
module 96 enables the boot-verify timer, module 95 restarts the canary,
the hub module runs `npm run build`. Low risk (no firmware), but
restarts services on a live cart — pick a quiet window.

### 1d. Merge PR #147 (`staging` → `master`)
All of the above is on `staging`. Merge once the rollout is validated.

---

## 2. Firmware-over-BLE reliability follow-ups (the forever path)

After the "one last USB flash" (1b), firmware updates are BLE-only. The
path works but has rough edges to smooth before it's the daily driver:

- **deploy.sh BLE post-flash verification — FIXED (`4464d94f`).** deploy.sh
  no longer false-fails on sealed BLE-`present` devices: it trusts the
  flash-job version-verify, accepts the `present` state as healthy without
  `json info`, and accepts `skipped:` on the broadcast restore/save. So a
  clean all-BLE deploy should exit 0. Only outstanding piece: run a clean
  all-BLE happy-path deploy to exit-0 to confirm end-to-end (the code fix is
  validated on the serial path; the §2 failure-injection runs exercised the
  flash, not a clean deploy.sh pass). `flash=ok` in the job result remains the
  authoritative success signal.
- **BLE-DFU runs at MTU 20 (~6.5 min/device).** The §3.3 MTU-247 bump
  did not take effect on the DFU connection (negotiated MTU 23 / chunk
  20) despite BL 0.8.0-10. A whole-fleet BLE flash at 20-byte chunks is
  ~12× slower than intended. Investigate whether the BL's DFU service
  requests the larger MTU or the host capped it.
- **Recovery guarantee — VALIDATED 2026-05-20 (failure injection on the
  bench).** The core "a failed BLE delivery never bricks a sealed
  device" guarantee was proven end-to-end:
  - **Mid-transfer interrupt:** killed a BLE-DFU at 20% (server restart
    mid-transfer → partial/invalid app). The device landed in
    `dfu_recovery` (AdaDFU advertising) — re-attemptable, NOT bricked.
    A subsequent BLE re-push completed `flash=ok` and the device
    recovered to a valid app. The recovery mechanism (invalid-app CRC →
    `DEFAULT_TO_OTA_DFU` → BLE-DFU) is interruption-cause-agnostic, so
    this covers server-crash / link-drop / power-loss equally.
  - **DFU-entry failure (observed serendipitously):** a flash failed at
    "Bootloader not found after 3 scan attempts" (transient BLE scan
    miss before the transfer began). The device stayed in app mode on
    its valid current firmware (`flash=error`, not bricked) — safe,
    just needs a retry.
  - **Canary fix held** across both flashes (the full re-push completed,
    which is impossible if the canary had restarted the server
    mid-flash).
  Both observed failure modes are safe. Two caveats remain for the
  daily driver: (a) flashes can fail to *start* transiently
  (DFU-entry scan miss) → deploy/operator needs retry logic; (b) the
  bench chip is NOT auto-recovered by the server ("Unknown device in
  DFU bootloader … Push firmware to recover") because it isn't an
  enrolled fleet member — real enrolled devices should auto-recover,
  but that wasn't exercised here and is worth confirming on a real
  fleet member.

## 3. Future: BL self-update over BLE (so a sealed BL is never stuck)

**Goal:** be able to update the bootloader itself over BLE, eliminating
the "must open the device for USB" requirement entirely. Not needed for
the current plan (1b updates the BL during the last USB pass), but it
would future-proof the fleet against a later BL fix.

**What's already there (no BL-version dependency — true on 0.8.0-4+):**
- `ble_dfu.py` already extracts + sends the DFU image type, including
  `bootloader = 0x02` (alongside `application`/`softdevice`).
- The Adafruit BL self-updates via the MBR (`SD_MBR_COMMAND_COPY_BL`)
  over whichever DFU transport, and the dual-transport BL runs DFU over
  BLE.

**The one missing piece (tractable, server-side):**
- `compile.py:generate_dfu_package` only emits an `"application"`
  manifest today. Add a `"bootloader"` variant (correct init packet +
  manifest type). This builder is already **pure-Python** ("avoids
  broken adafruit-nrfutil on Python 3.13"), so the dropped
  `adafruit-nrfutil genpkg` does NOT block this path.

**Why it's gated behind careful validation (NOT just code):**
- A BL update over BLE is riskier than an app update. The slow BLE
  transfer lands in a staging bank (recoverable if it drops — live BL
  intact), but the final MBR copy that swaps the new BL into the BL
  region has a small power-loss window — if interrupted there, a sealed
  device **bricks with no USB/SWD recovery**.
- §3.1's watchdog can't help (it lives in the BL; a corrupt BL can't run
  it).
- **Untested:** this session validated only *app*-over-BLE-DFU.
  BL-over-BLE needs its own bench campaign (many cycles + deliberate
  mid-transfer-drop + power-loss-during-MBR-copy thought experiment)
  before it's trusted on sealed hardware.

---

## 4. Deferred / won't-fix

- **§1.1 full-fleet delivery measurement** — bench is 20/20; the
  multi-device, festival-range number needs the deployed fleet (rides
  on 1a).
- **§6 extended-adv** — won't-do on current hardware (ext-adv only on
  the audio controller). Revisit only if a long-command need appears;
  path documented in the archive.

---

## 5. Housekeeping

- Old doc archived at `docs/archive/OPEN_ISSUES_2026_05_19.md`.
- `docs/SAFETY.md`, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` — already
  refreshed 2026-05-19.
