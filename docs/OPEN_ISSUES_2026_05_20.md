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

### 1a. Fleet firmware: b179 → b190
Picks up §3.1 firmware clear, §3.4 fresh-build reformat, §1.4 button
debounce, gossip-ACK firmware bits, RxSlot packing. Sealed devices →
BLE-DFU via `deploy.sh`. One device at a time; test chip first;
≥75 s uptime between resets (60-second rule). Validate big_bucket's
phantom presses stop after its flash (closes §1.4).

### 1b. Fleet bootloader: 0.8.0-7 → 0.8.0-10
Adds §3.1 GPREGRET app-handshake watchdog. Higher-stakes than firmware
— goes through `deploy-bootloader.sh` (verifier-gated). The carts'
BL is confirmed 0.8.0-7; the 6 other BLE-only members are unconfirmed
and can't be checked without UF2-mode access (sealed). Decide whether
the recovery improvement justifies a BL rollout to sealed devices
before the next install.

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
