# Open issues snapshot — 2026-05-19

State of unresolved work at the end of the marathon session that
shipped PR #144 hardening + lemon-cart canary scaffold. Living
document; cross-link from a successor instead of editing in place
once items get addressed.

---

## 1. Critical operational

### 1.1 BLE broadcast delivery ceiling ~80% per-broadcast

**Status: Phase 2 SHIPPED (server side).** Discovery extracts
`last_cmd_id` from each BLE device's scan-response manufacturer data
and stores it on the `Device`. After every discovery cycle the
FleetManager hands the snapshot to `FleetBroadcaster.rebroadcast_to_laggards()`,
which re-emits the cached `_last_command` with the SAME `_command_id`
for any device whose ACK lags. Capped at `REBROADCAST_MAX_ATTEMPTS = 3`
per (device, command_id) to bound radio time on a permanently-out-of-
range device; the per-device counter clears on each fresh
`broadcast_command` (new logical command resets the budget). Field
validation pending — bench measurement of per-broadcast delivery rate
after Phase 2 vs. pre-Phase-2 baseline.

**Symptom (original, pre-fix):** 4 of 20 broadcasts hit zero devices
on the bench measurement (2026-05-19 evening). Systemic misses were
**broadcaster-side**, not random per-emit RF — all devices missed the
*same* broadcasts.

**Prior state (shipped earlier):**
- Items #1 (multi-slot rxBuffer), #2 (command_id idempotency), #3
  (per-source seq ring), and scan-duty-cycle 50%→90% all SHIPPED.
- Reception per-emit improved 42% → ~70% within successful broadcasts.
- Per-broadcast delivery 80%. Adding more emits / wider duty cycle
  didn't help the 20% all-emits-missed case — that's what gossip-ACK
  re-broadcast targets.

**Workaround (no longer needed):** the operator's "tap it again"
habit is now redundant on the second pass — the server will retry
within ~60 s of the missed broadcast.

---

### 1.2 Scene system → generator-only refactor

**Status: server + console SHIPPED 2026-05-19.** `/api/scenes/*`
routes, `scene_cursor` module, `scenes` module, and the test suite
are deleted from `blinky-server`. The `~/.local/share/blinky-server/scenes/`
directory is orphaned on disk; safe to delete but the server no longer
manages it. `ScenesPanel.tsx` + tests + ~145 lines of CSS removed
from `blinky-console`; `SCENE_VISIBLE_SETTINGS` / `isSceneVisible`
dead code removed from `settingsMetadata.ts`. `PacketType.SCENE = 0x02`
is kept in `protocol.py` only as a mirror of `BleProtocol.h` (the
firmware enum is unchanged); no Python code emits SCENE packets.

**Decision 2026-05-19:** eliminate scenes entirely. Direct generator
selection only; hue rotation managed via Hub UI sliders.

**Shipped:**
- Lemon-cart: light button cycles `fire/water/plasma/audio`
  (commit `e521e42` in the lemon-cart repo). Cursor moved to
  client-side `/run/lemoncart/generator-cursor`.
- blinky-server: scene routes + module + library + tests deleted.
- blinky-console: `ScenesPanel` deleted; CSS + dead settings-metadata
  helpers removed.

**Still pending (lemon-cart repo):**
- Hub UI: scene chip section should be removed; **new** hue rotation
  speed slider + absolute hue position slider added.

**Why the rush to refactor:** scene commands like
`scene water static 0.0 0.66` (28 bytes) exceed the 31-byte legacy BLE
adv limit. BlueZ silently promotes them to extended adv, which the
firmware's legacy scanner doesn't watch. `gen <name>` (≤10 bytes)
fits comfortably.

---

### 1.3 Cascade bug — UF2 verify timeout → BLE-DFU auto-recovery

**Status: FIXED in PR #145** (commit `c31ba4e2`). `FleetManager` now
tracks `_recent_uf2_writes` separately from `_recent_flash_attempts`;
a successful UF2 byte-write (job reached VERIFYING with
`write_completed_at` set) blocks BLE-DFU auto-recovery on the same
canonical device for `UF2_WRITE_AUTO_RECOVERY_BLOCK_S = 1800 s` (30
min) regardless of whether the verify phase converged. Operator path
(`force=True`) bypasses. Audit log rebuilds the tracker on restart so
the guard survives server bounces.

**Reference:** [[project-deploy-flash-cascade-bug]] (memory).

**Original symptom:** UF2 write succeeded, post-flash version-verify
timed out at 5 minutes (some path was slow), server marked the flash
FAILED, the auto-recovery loop then BLE-DFU'd the device — which
overwrote the working UF2 write. Audit log on 2026-05-19 showed ~5
cascade events during one session, costing ~30 min total.

---

### 1.4 Big_bucket phantom button presses

**Status: firmware mitigation SHIPPED (pending flash).**
`GeneratorButton::poll()` now does two-layer debounce: (a) a
sustained-level filter requiring `STABLE_SAMPLE_COUNT = 3` consecutive
agreeing samples before recognizing a level change (≈50 ms at 60 Hz
poll cadence, absorbs short EMI pulses); (b) `DEBOUNCE_MS` bumped
200 → 500 ms (absorbs longer ones). Combined, the chain rejects
sub-50 ms EMI spikes outright and bounds the accepted press rate to
≤2 Hz, which is well above any operator-press cadence. Effective
after the next deploy.sh that touches big_bucket. If phantom presses
persist after the flash, fall back to registry option #1.

**Symptom (original):** big_bucket's generator changes unprompted,
suspected phantom presses on the firmware-side GPIO button (D1).

**Hardware:** internal pull-up + 200 ms debounce was in place
pre-fix. Suspected cause: long unshielded wire picking up EMI from
the WS2812B data line, or a flaky physical button.

**Remaining workarounds (if the firmware fix is insufficient):**
1. Disable the firmware button via registry: set `buttonPin: 0` in
   `big_bucket_v1.json`, push config + reboot. Instant fix.
2. Add hardware filtering (100 nF cap GPIO→GND).

---

## 2. PR #144 review items still open

| Item | File | Issue | Status |
|---|---|---|---|
| 7 | `tests/test_fleet_ble.py` | `bc.COMMAND_REEMIT_HOLD_MS = 0.0` instance-attribute override is fragile. | ✅ Replaced with `_fast_broadcaster(monkeypatch)` helper that patches the class attribute. |
| 8 | `tests/test_registry_jsons.py` | Only checked `ledType` validity. | ✅ Added per-file checks for required-field presence, deviceId↔filename match, ledWidth/ledHeight positive (with 2048 sanity cap), and orientation/layoutType enum-range. |
| 11 | `blinky_server/ble/protocol.py` | Docstring claimed 238-byte ceiling without flagging the firmware's 21-byte legacy-adv effective limit. | ✅ Docstring now documents both ceilings and the silent-on-air failure mode. |
| (n/a) | `BleScanner.cpp` `RxSlot` struct | `uint16_t len` misaligned at byte offset 1. | ⏸ Perf nit; not worth packing now per the original review. |

---

## 3. Bootloader recovery gaps

### 3.1 BL doesn't auto-DFU on app-crashes-pre-BLE-init

**Status: SHIPPED in b183 + BL `0a2b140` (2026-05-19).** Bench-verified
on `659C8DD3ADF84A33`: BL flash via `deploy-bootloader.sh` succeeded,
b183 firmware booted to steady state (uptime >60s, fps ~629), then
power-cycled cleanly back to app mode (no false DFU). The
`verify_bootloader.py` invariant still passes — the new force-DFU path
lives outside any `if (_ota_dfu)` branch and reaches
`bootloader_dfu_start()` through the existing dual-transport block.

**Implementation summary:**
* **RAM layout chosen:** option (b) — packed into a new 4-byte word at
  `0x20007F78`, immediately below the existing `dbl_reset_mem`
  (`0x20007F7C`). Sentinel `0xCAFE00` in the upper 24 bits gates against
  RAM garbage on cold boot; the low byte is the count. Same
  "collision avoidance" property as the existing magic — both linkers
  empirically don't reserve this address.
* **BL change** (`bootloader/src/src/main.c`, commit `0a2b140`):
  counter check after `APP_ASKS_FOR_SINGLE_TAP_RESET`; if
  `attempts >= BOOT_ATTEMPT_THRESHOLD` (3) and sentinel valid, force
  `dfu_start = 1` and set `_ota_dfu` iff `!VBUSDETECT` (so cabled
  devices favor UF2 LED hint, sealed devices favor BLE). Increment
  happens between `(*dbl_reset_mem) = 0;` and `bootloader_app_start()`.
* **Firmware-side clear:** new `SafeBootWatchdog::clearBootAttemptCounter()`
  writes `0xCAFE0000` (sentinel | count=0) with `__DSB()/__ISB()`,
  called from the existing 60 s `markStable()` block in
  `blinky-things.ino`.
* **Forward compatibility:** older fleet BLs ignore the address, so
  the firmware clear is a harmless no-op on devices that haven't
  picked up the new BL yet. New BL is also backward compatible — a
  pre-§3.1 firmware just never clears, and the counter still won't
  trip on legitimate boots because the counter resets to garbage
  (sentinel mismatch) on every cold-boot RAM init.

**Deferred validation:** intentional crash-loop test to confirm the
3-strikes-forces-DFU path. Not run on the bench because the only
SWD-recoverable chip is in active use; the path is exercised by the
existing increment logic (verified by inspecting steady-state behavior
across power cycles) but not yet end-to-end on a real crash.

**Design constraints captured during 2026-05-19 survey** (kept for
reference; the chosen implementation resolves each):

* **RAM layout for the handshake marker.** The existing `dbl_reset_mem`
  at `0x20007F7C` is a 4-byte word with exact-match magic semantics
  (`DFU_RAM_MAGIC_UF2` / `_BLE` / `_QSPI`). Adding a boot-attempt
  counter requires either (a) carving a new region from the
  SoftDevice-reserved zone immediately below `0x20007F7C`, (b)
  packing the counter into the existing word via a non-conflicting
  encoding, or (c) placing it in the bootloader's `NOINIT` region at
  `0x20007F80+` (currently holds `m_peer_data` / `m_peer_data_crc` —
  variable offset depends on link order). Option (a) requires
  changes to `bootloader/src/linker/nrf52840.ld`. The firmware's
  linker (`nrf52840_s140_v7.ld`) does NOT reserve any of these
  addresses — the existing `dbl_reset_mem` works because the
  firmware's BSS/heap empirically don't land at `0x20007F7C`, AND
  because magic writes happen immediately before `NVIC_SystemReset`
  with `__DSB(); __ISB()` (so the BL reads fresh RAM before the next
  firmware run touches it). Any new shared address inherits the same
  "this works by collision avoidance" property.

* **Increment timing.** BL must increment the counter on every
  "about to jump to app" path (line `main.c:236`,
  `bootloader_app_start()`). Includes the post-DFU-completion path.
  Does NOT include DFU-mode entry (no app jump).

* **Clear timing.** Firmware should clear the counter at the same
  point `SafeBootWatchdog::markStable()` fires (60 s uptime) — that's
  the existing "I successfully booted" milestone. Earlier than
  60 s and legitimate-but-slow boots false-positive into DFU.

* **`verify_bootloader.py` invariant.** The new path that forces
  `dfu_start = 1` from the counter check still reaches
  `bootloader_dfu_start()` via the existing dual-transport block,
  which calls `usb_init()` unconditionally. Invariant preserved IF
  the new code lives outside any `if (_ota_dfu)` branch. The
  verifier's static check should still pass without modification.

* **§3.2 interaction.** §3.2 (prefer-UF2-when-USB) is **firmware-side**
  (writes RAM magic). When the BL forces DFU via this §3.1 mechanism,
  there's no firmware involvement — the BL must do its own
  VBUSDETECT check to pick UF2 hint vs BLE hint. With the existing
  dual-transport design, both transports come up anyway; the
  `_ota_dfu` flag only affects the LED hint.

**Reference:** [[project-bl-no-app-crash-fallback]].

**Original symptom:** `check_dfu_mode()`'s `DEFAULT_TO_OTA_DFU` only
fires when the app slot is **invalid by CRC**. A crashy-but-valid
app boots, HardFaults pre-BLE-init, BL re-runs, sees valid app,
jumps, crashes again. No auto-DFU fallback. Cost a controller
2026-05-19.

**Mitigation in the meantime:** the existing
`RebootFrequencyCounter` (flash-backed, LittleFS counter) catches
crashes that survive past `configStorage.begin()` + the
`checkAndIncrement()` call at `.ino:209`. The §3.1 gap is for
crashes BEFORE that call (very early `setup()` or even static-init
fiasco). The §3.4 fresh-build reformat shipped this session reduces
the static-init / LittleFS-corruption surface, indirectly shrinking
the §3.1 surface too.

---

### 3.2 BL prefer UF2 over BLE-DFU when USB cable detected

**Status: SHIPPED in b181+** (commit `ac254194`). The fix lives in
*firmware* (not the bootloader, as the original entry implied) —
specifically `SafeBootWatchdog::enterRecoveryBootloader` now reads
`NRF_POWER->USBREGSTATUS.VBUSDETECT` and writes the UF2 RAM magic
(0xBEEF0057) instead of the BLE-DFU magic (0xBEEF00A8) when a USB
cable is providing VBUS. Sealed devices with no USB fall through to
BLE-DFU as before, so the autonomous-recovery contract for
installed sculptures is unchanged.

Bench-validated 2026-05-19 on `659C8DD3ADF84A33`: device flashes
cleanly, boots through the §3.4 fresh-build path, healthy fps. The
functional change is dormant in normal operation (only fires on
the 5-strike `RebootFrequencyCounter` quarantine path); field
validation happens whenever the next auto-recovery actually fires.

**Reference:** [[project-bl-prefer-uf2-when-usb]].

**Original symptom:** auto-fallback always picked BLE-DFU (~5.5 min).
When a USB cable was connected at fallback time, UF2 mass-storage
path would have recovered in ~30 seconds.

**Caveat:** doesn't help **sealed devices** [[feedback-enclosed-devices-no-physical]]
where USB isn't accessible. But improves bench recovery dramatically.

---

### 3.3 BLE-DFU MTU bump — 10x transfer speedup opportunity

**Status: SHIPPED 2026-05-19** (BL submodule commit `2cbe16d`,
staged at `bootloader/update-bootloader-qspi-ota-default.uf2`
sha256 `aa50445e…`). The Legacy DFU's hardcoded 20-byte payload
came from the SoftDevice default ATT_MTU=23 (23 minus 3-byte ATT
overhead). Three coordinated changes lift the ceiling:

* `bootloader/src/src/main.c` and
  `bootloader/src/lib/sdk11/components/libraries/bootloader_dfu/dfu_transport_ble.c`:
  `BLEGATT_ATT_MTU_MAX` 23 → 247. SD config + client MTU exchange
  both now honour up to 247.
* `.../hci_mem_pool_internal.h`: `RX_BUF_SIZE` 32 → 256 to hold the
  larger DFU data writes (~244 bytes at MTU=247). +1.8 KB RAM cost,
  well within the BL's 224 KB headroom.
* `blinky-server/blinky_server/firmware/ble_dfu.py`: removed
  hardcoded `mtu = 20`; the inner transfer now reads
  `client.mtu_size - 3` clamped to `[20, 244]`. Old bootloaders
  still work transparently (negotiated MTU stays ≤23 → chunk 20).

Verify-bootloader.py invariants still pass. Bench-validated on
`659C8DD3ADF84A33` 2026-05-19: new BL `0.8.0-7-gc79626c-dirty`
self-update via `deploy-bootloader.sh` succeeded; device booted
firmware b181 cleanly post-update. Actual transfer-time speedup
not yet measured (would need to BLE-DFU something with the new BL
running and time it; deferred to a future session).

Important for [[feedback-enclosed-devices-no-physical]]: sealed
devices can't shortcut to UF2 via cable. BLE-DFU is their only
recovery path; making it faster is high-value.

---

### 3.4 Stale-flash crash on big version upgrades

**Status: firmware mitigation SHIPPED in b180+** (commit `116090d9`).
`ConfigStorage::begin` (nRF52 branch) now does a freshness check:
the `/.fw_build` marker file records the FIRMWARE_BUILD that last
successfully booted. On a marker mismatch (first boot of any new
build), the firmware:

  1. Snapshots `/config.bin` (preserves device identity)
  2. `InternalFS.format()` — clears all accumulated LittleFS state
  3. Re-writes `/config.bin` from snapshot
  4. Writes the marker LAST (idempotent on power-cut between steps 1–3)

Bench-tested 2026-05-19 on `659C8DD3ADF84A33`: deploy of b180 to a
safeMode chip booted cleanly through the fresh-build path; subsequent
`save` and `json info` commands work normally. Field validation
against the original symptom (chip with stale b162-era LittleFS
state) pending; the design is defensive enough that breakage falls
through to existing safeMode → re-upload recovery rather than a brick.

**Reference:** [[project-stale-flash-state-after-upgrade]].

**Original symptom:** Fresh chips upgraded across many versions
(e.g. b162 → b179) crashed during first configured boot due to
residual non-app flash regions. Self-healed via
`RebootFrequencyCounter` quarantine → safeMode → re-upload, but ate
~30 s of crash-recovery time per device. The operator workaround
was "bench-burn a cycle before installing"; the marker-driven
reformat eliminates the need for that.

---

## 4. Service hardening gaps (production reliability)

### 4.1 Canary probe coverage incomplete

**Shipped:** `lemoncart-canary.service` with 4 probes — hostapd,
blinky-server, dnsmasq, lemoncart-buttons.

**Probes to add (TODO comment in `lemoncart-canary` script):**
- **End-to-end fleet ping** — emit synthetic "canary" command,
  verify `broadcaster.command_id` advances and at least one device's
  gossip-ACK adv reflects it within N seconds. Catches the
  Section 1.1 silent-broadcaster-broken state.
- **pipewire (user service)** — verify audio sinks exist
  (`pactl list short sinks`). Pipewire crash today shows audio
  routes silent with no upstream alert.
- **bluetooth adapter health** — verify `rfkill list` shows
  `Soft blocked: no` and `hci0` is up. Adapter rfkill events
  during heavy traffic can mute BLE.
- **station-api** — HTTP probe with a known query (e.g. mix state).

---

### 4.2 No `sd_notify` watchdogs on critical services

`Restart=on-failure` covers crashes. `WatchdogSec` + service-side
`sd_notify(WATCHDOG=1)` would catch deadlocks ("running but stuck").
`blinky-server` and `station-api` are the highest-value targets.

**Adds:** ~5 lines of Python per service to ping the watchdog from
the main loop. WatchdogSec in the unit file. No protection against
the deadlock-class failure today.

---

### 4.3 No memory caps on services

`MemoryMax=` drop-ins would let systemd OOM-kill a leaked service
(triggering `Restart=on-failure`) rather than letting it slowly
consume the box. Reasonable defaults: 256 MB for blinky-server,
128 MB for station-api, 64 MB for canary. No observed leaks tonight,
but cheap insurance for an unattended event.

---

### 4.4 No boot-time service verification

After a power cycle (real risk at festivals on UPS), no service
checks "did everything come back up?" The canary catches steady-state
failures but not boot-time start-failures of services it doesn't
probe yet.

**Proposed:** `lemoncart-boot-verify.service` — `After=multi-user.target`,
runs once 60s after boot, checks every critical unit is `active`,
logs loud + reboot-once on any failure. Catches the
"comes-back-from-power-loss-but-X-never-started" class.

---

## 5. Hub UI work (depends on Section 1.2 refactor)

Section 1.2's server + console cleanup landed 2026-05-19. The
remaining work is in the lemon-cart repo (`~/lemon-cart/`).

**Deployable today (no protocol changes):**
- **Generator chip row** — replaces scene chips. Routes to
  `POST /api/fleet/generator/{name}`; 4 chips fire/water/plasma/audio,
  each command is `gen <name>` (≤10 bytes on wire). Fits the
  legacy-adv ceiling comfortably.

**BLOCKED on protocol-length resolution:**
- **Hue rotation speed slider** — fleet-wide via
  `set effectRotationSpeed <value>` (28 bytes on wire).
- **Absolute hue position slider** — `set effectHueShift <value>`
  (same length range).

Both slider commands exceed the firmware scanner's 21-byte
legacy-adv effective ceiling — BlueZ silently promotes them to
extended adv, which the firmware doesn't watch (the same root cause
as the scene-system command overflow). **Two paths to unblock:**

1. **Short-name aliases** in firmware: rename the broadcast paths to
   use `huespeed` / `hueshift` (existing short names, already
   recognised by the firmware's `set` command). Server-side change:
   emit `set huespeed <v>` (16 bytes) instead of
   `set effectRotationSpeed <v>` (28 bytes). No firmware change
   required because the short names already work. **This is the
   cheaper option and should be tried first.**
2. **Firmware extended-adv scanning** (Section 6) — lifts the ceiling
   for everything. Not a one-liner (BSP patch + ~half-day of work);
   see Section 6 for actual scope.

Until ONE of these lands, don't ship the slider Hub UI work — the
broadcast would silently no-op. The generator chip row CAN ship now.

---

## 6. Firmware extended-adv scanning (cross-cutting)

The 21-byte effective payload ceiling for legacy BLE adv constrains:
- Scene commands (Section 1.2) — abandoned, going generator-only
- Set commands with long names (Section 5)
- Future protocol extensions

**Status: research complete, NOT a one-liner.** Investigation
2026-05-19: Adafruit Bluefruit (`Bluefruit52Lib/src/BLEScanner.{h,cpp}`)
does not expose a `useExtendedAdv()` method. The `start()` path
fixes `_param.extended = 0` in the constructor (with a literal
`// TODO Extended Adv on secondary channels`) and allocates the scan
buffer at `_scan_data[BLE_GAP_SCAN_BUFFER_MAX = 31]`. To enable
extended adv:

1. **BSP-level patch** of `BLEScanner.cpp`: resize `_scan_data` from
   31 → ≥255 (`BLE_GAP_SCAN_BUFFER_EXTENDED_MIN`), add a public
   `useExtendedAdv(bool)` that flips `_param.extended` and toggles
   `_report_data.len` between the two sizes. New `patches/` file
   + re-apply on BSP update (precedent: `patches/tinyusb-cdc-no-overwritable-fifo.patch`).
2. **Firmware-side** `Bluefruit.Scanner.useExtendedAdv(true)` in
   `BleScanner::begin` BEFORE `start(0)`.
3. **handleReport** path needs to tolerate the larger frame layout
   (SoftDevice may deliver fragments via `report_incomplete_evts`
   if a single extended adv exceeds the 255-byte buffer; we'd need
   to either ignore those or accumulate them).
4. **Verify with bench chip + signal generator** before fleet rollout.

**Cost:** ~half-day investigation + patch + firmware + BSP-patch
install/CI hook + bench validation. Plus the ~55 min BLE-DFU rollout
to fleet (eased by Section 1.1 gossip-ACK re-broadcast which makes
delivery more reliable).

**Workaround in the meantime:** keep fleet commands under 21 bytes
on-wire. `gen <name>`, `effect <mode>`, `set <short> <value>` all
fit. Long-named settings (`effectRotationSpeed = 28 bytes`) need
either a shorter name or this extended-adv work.

---

## 7. Devices currently on outdated state

All 10 fleet devices on `b179-699c3025-dirty` as of end of session.
No outdated devices.

Bootloaders on the fleet:
- Carts (cart_inner, cart_outer): BL `0.8.0-7-gc79626c` per CLAUDE.md
- Hat: not verified post-flash today, likely `0.8.0-7`
- Other 6 BLE-only fleet members (umbrella, big_bucket, bucket_v3,
  tube_v2, 3× long_tube): BL version unconfirmed (would need a per-
  device check via the BL's `INFO_UF2.TXT` over UF2 mode, which
  requires double-tap reset — **not possible on enclosed devices**).

If a BL upgrade is needed for the recovery improvements in Section 3,
it would need to happen on the bench before sealing.

---

## 8. Documentation drift

- `docs/SAFETY.md` — ✅ rewritten 2026-05-19 as a tight 4-invariant
  index pointing at current code paths. The COMMAND_V2 / cmd_id-ring
  details live in `docs/BLE_FLEET_RELIABILITY_PLAN.md` and the
  `BleProtocol.h` / `advertiser.py` docstrings; out of scope for the
  flashing-safety doc.
- `docs/BLUETOOTH_IMPLEMENTATION_PLAN.md` — ✅ scene→generator
  framing updated (sole "scene" mention in the use-case bullet).
  Body otherwise covers NUS / BLE DFU / fleet discovery — not
  scene-specific.
- This document — itself should get rolled into a successor at the
  start of the next session. Multiple §1-§3 items have been closed
  in-place with status notes; once the file's "remaining-work
  density" drops below ~30%, succession is cleaner than further
  in-place edits.

---

## Cross-reference index

| Topic | Doc / Memory |
|---|---|
| Cascade bug | [[project-deploy-flash-cascade-bug]] |
| BL no app-crash fallback | [[project-bl-no-app-crash-fallback]] |
| BL prefer UF2 when USB | [[project-bl-prefer-uf2-when-usb]] |
| Stale flash on upgrade | [[project-stale-flash-state-after-upgrade]] |
| BlueZ address rotation | [[project-bluez-addr-rotation]] |
| Enclosed devices no physical | [[feedback-enclosed-devices-no-physical]] |
| No serial at events | [[feedback-no-serial-at-event]] |
| Canary probe correctness | [[feedback-canary-probe-correctness]] |
| hostapd country stuck | [[project-hostapd-country-stuck]] |
| BLE fleet plan | `docs/BLE_FLEET_RELIABILITY_PLAN.md` |
| Flash lockdown plan | `docs/FLASH_LOCKDOWN_PLAN.md` |
| Sculpture BLE recovery | `docs/SCULPTURE_BLE_RECOVERY_PLAN.md` |
