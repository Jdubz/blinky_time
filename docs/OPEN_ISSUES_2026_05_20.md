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

## 1. Production rollout (the main remaining lever)

Everything above is on `staging` / committed to the lemon-cart repo but
**not yet on the production fleet or carts.** This is a careful,
authorize-each-step operation — sealed devices recover only via
BLE-DFU, and the session's brick incident is the reason for the caution
([[feedback-flash-safety-policy]], [[feedback-no-unauthorized-retries]],
[[feedback-test-chip-first]]).

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
- **deploy.sh post-flash steps don't verify BLE devices.** Per-device
  commands (`json info`, `restore_runtime_settings`, `save`) require
  `state==CONNECTED`; sealed BLE devices sit at `present` (advertising,
  not GATT-connected) and 409 "not connected". The flash JOB's own
  version-verify (`flash=ok`) is the authoritative check and passed —
  but deploy.sh's *post-flash* settings-restore + json-info health
  re-check can't run on a BLE-`present` device. For an all-BLE fleet
  deploy, deploy.sh will report failure at the restore step even though
  the flash succeeded. Needs either: deploy.sh skips per-device
  post-steps for non-connected BLE devices (trust the flash job +
  fleet-broadcast the restore/save), or the server connects each BLE
  device on demand for these.
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
first; ≥75 s uptime between resets (60-second rule). **Caveat:** expect
deploy.sh to report failure at the post-flash restore/verify step for
BLE devices (see audit above) — confirm `flash=ok` in the job result is
the real success signal until the post-step BLE gap is closed. Validate
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

- **deploy.sh can't verify BLE devices post-flash.** Per-device commands
  (`json info`, `restore_runtime_settings`, `save`) require
  `state==CONNECTED`; sealed BLE devices sit at `present` and 409. So an
  all-BLE deploy reports failure at the restore step even though the
  flash JOB's own `flash=ok` is authoritative. Fix options: deploy.sh
  trusts the flash-job result for non-connected BLE devices and fleet-
  broadcasts the restore/save (instead of per-device), OR the server
  connects each BLE device on demand for the post-steps. **Until fixed,
  the operator's success signal for a BLE firmware update is `flash=ok`
  in the job result, NOT deploy.sh's exit code.**
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
