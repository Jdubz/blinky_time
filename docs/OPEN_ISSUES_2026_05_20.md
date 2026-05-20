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

### 1b. Fleet bootloader: 0.8.0-7 → 0.8.0-10 — NOT POSSIBLE on sealed devices
`deploy-bootloader.sh` is **USB/UF2-only** (enters DFU over serial,
copies to the mounted UF2 volume — no BLE path). Sealed devices have no
USB access, so the §3.1 GPREGRET watchdog BL **cannot be deployed to the
sealed fleet** with current tooling. The BLE-OTA-of-BL alternative needs
the `.zip` bundle, which is also broken (`adafruit-nrfutil` dropped
`genpkg`). So the sealed fleet stays on its current BL (0.8.0-7, which is
fine); 0.8.0-10 only reaches devices given USB access before sealing.
deploy-bootloader.sh itself is now validated on the bench (headless fix).

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

## 2. Deferred / won't-fix

- **§1.1 full-fleet delivery measurement** — bench is 20/20; the
  multi-device, festival-range number needs the deployed fleet (rides
  on 1a).
- **§6 extended-adv** — won't-do on current hardware (ext-adv only on
  the audio controller). Revisit only if a long-command need appears;
  path documented in the archive.

---

## 3. Housekeeping

- Old doc archived at `docs/archive/OPEN_ISSUES_2026_05_19.md`.
- `docs/SAFETY.md`, `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` — already
  refreshed 2026-05-19.
