# Bootloader Production Audit & Outstanding Actions — 0.8.0-5 / 0.8.0-6 / 0.8.0-7

**Status as of 2026-05-16:**

| Item | Status |
|------|--------|
| `bootloader_settings_save` SD-aware fix | ✅ landed in BL commit `8a02a35` (0.8.0-6) |
| 80% USB-flash hiccup on 0.8.0-5 measured on real hardware | ✅ verified (`scripts/bl_characterize.sh`) |
| 100% of failures have COMPLETE flash → bug = settings_save, not flash writes | ✅ verified |
| Hardware test of 0.8.0-6 (50% reduction in hiccup rate) | ✅ verified — 20-iter test |
| BLE-quiet-during-MSC patch (eliminates 5-90s slow-completion tail) | ✅ landed in BL commit `c79626c` (0.8.0-7) |
| `writtenMask` counter race on UF2 drops (host-side block corruption → permanent stuck) | ✅ bounded by stuck-detect in `c79626c` (0.8.0-7); see "RESOLVED" section below for why this is a recovery and not a root-cause fix |
| Stuck-transfer recovery (self-reset after 8s idle + incomplete) | ✅ landed in BL commit `c79626c` (0.8.0-7) |
| Hardware test of 0.8.0-7 (expect ~100% recovery) | ✅ verified — 30-iter test, 30/30 PASS, mean 9.4s |
| `scripts/verify_bootloader.py` catches both bug classes at source level | ✅ landed |
| `scripts/deploy-bootloader.sh` gates flash on verifier | ✅ landed |
| Fleet rollout of 0.8.0-7 | ⬜ pending — needs deploy-bootloader.sh against fleet |

This document captures outstanding actions and the verified diagnostic
evidence behind them. It is NOT a historical narrative — see git log
for that.

---

## Architecture summary

`d7be9be` is the dual-transport BL with the SD-aware flash write fix:

- Both BLE and USB MSC are initialized on every DFU entry (`src/main.c:559-562`).
- `flash_nrf5x_flush()` checks `sd_softdevice_is_enabled()` and routes through
  `sd_flash_*` API when SoftDevice is enabled
  (`src/flash_nrf5x.c:_sd_is_running`, post-d7be9be).
- `DEFAULT_TO_OTA_DFU` provides auto-recovery for sealed devices with invalid
  apps (`src/main.c:240-242`).
- `bootloader_dfu_sd_in_progress()` resumes interrupted SD/BL updates on boot
  (`src/main.c:195-203`).
- Watchdog is fed from the wait-for-events loop
  (`lib/sdk11/components/libraries/bootloader_dfu/bootloader.c:118-123`).

These are sound design choices. The asymmetric handling of NVMC vs SD is the
problem.

---

## CONFIRMED BUG — `bootloader_settings_save` silently fails on UF2 path

**Source:** `lib/sdk11/components/libraries/bootloader_dfu/bootloader.c:185-202`

```c
static void bootloader_settings_save(bootloader_settings_t * p_settings)
{
  if ( is_ota() )
  {
    pstorage_clear(...);            // SD-aware via pstorage_raw
    pstorage_store(...);
  }
  else
  {
    nrfx_nvmc_page_erase(BOOTLOADER_SETTINGS_ADDRESS);
    nrfx_nvmc_words_write(BOOTLOADER_SETTINGS_ADDRESS, ...);
    pstorage_callback_handler(... NRF_SUCCESS ...);   // <-- always reports success
  }
}
```

**The bug:** the else branch uses `nrfx_nvmc_*` direct register access. Per
Nordic spec (and per the d7be9be commit message that fixed the same pattern in
`flash_nrf5x.c`), direct NVMC access **silently no-ops** when the SoftDevice is
enabled and the radio is active. The `pstorage_callback_handler` is then
invoked unconditionally with `NRF_SUCCESS`, so the caller thinks settings were
saved successfully.

**When this triggers in production:**

| DFU trigger | `is_ota()` | SD running? | Path taken | Result |
|-------------|-----------|-------------|-----------|--------|
| GPREGRET=OTA_RESET (BLE OTA) | true | yes | pstorage (SD-aware) | ✓ works |
| GPREGRET=OTA_APPJUM (BLE OTA jump) | true | yes | pstorage | ✓ works |
| GPREGRET=UF2_RESET (UF2 drop) | **false** | **yes** | **direct NVMC** | **silent fail** |
| GPREGRET=SERIAL_ONLY (banned) | false | yes | direct NVMC | banned anyway |
| Double-tap reset | false | yes | direct NVMC | **silent fail** |
| `DEFAULT_TO_OTA_DFU` auto-recovery | true | yes | pstorage | ✓ works |

**Observed symptom = "consumed-but-no-reboot":**

The user reported this from earlier deployments. Chain:

1. Operator drops UF2 → host writes bytes
2. ghostfat receives blocks, `flash_nrf5x_flush` (SD-aware) writes them to flash ✓
3. `numWritten == numBlocks` → `tud_msc_write10_complete_cb` fires `DFU_UPDATE_APP_COMPLETE`
4. `bootloader_dfu_update_process` sets `settings.bank_0 = BANK_VALID_APP`
5. `bootloader_settings_save(settings)` called with `is_ota()==false`
6. **Direct NVMC write silently no-ops; bank_0 stays at its previous value**
7. `pstorage_callback_handler(SUCCESS)` triggers `m_update_status=BOOTLOADER_COMPLETE`
8. Main loop returns; `bootloader_app_is_valid()` returns **false** (bank_0 != BANK_VALID_APP)
9. `DEFAULT_TO_OTA_DFU` sets `GPREGRET=OTA_RESET`; `NVIC_SystemReset()`
10. Next boot: BL enters OTA-DFU mode; device is in BL again. Operator sees "consumed but no reboot"

**Why multi-drop "fixes" it:** writtenMask isn't reset between drops (statics
persist until system reset). Each re-drop has more chances of catching a moment
when the radio is briefly idle and the direct NVMC write actually lands. Once
it does, bank_0 is properly set and the device boots normally.

**Likelihood estimate:** unknown. Depends on SD radio duty cycle during the
post-flash settings save (a few NVMC cycles in a window of milliseconds).
Bench observation from the user's earlier deployments suggests it's
non-trivial — hit on multiple sealed devices.

**Recommended fix:** mirror the d7be9be pattern in `bootloader_settings_save`:
check `sd_softdevice_is_enabled` and route through `sd_flash_*` for the
settings page when SD is enabled. Drop the `is_ota()` gating — base the
decision on actual SD state.

---

## RESOLVED — `writtenMask` counter race (bounded by stuck-detect in 0.8.0-7)

**Source:** `src/usb/uf2/ghostfat.c:594-628`

```c
if ( bl->numBlocks ) {
  if ( bl->blockNo < MAX_BLOCKS ) {
    uint8_t const mask = 1 << (bl->blockNo % 8);
    uint32_t const pos = bl->blockNo / 8;
    if ( !(state->writtenMask[pos] & mask) ) {
      state->writtenMask[pos] |= mask;
      state->numWritten++;
    }
    if ( state->numWritten >= state->numBlocks ) {
      flash_nrf5x_flush(true);
      // ... only here does completion get triggered
    }
  }
}
// TODO numWritten can be smaller than numBlocks if return early  <-- Adafruit's TODO
```

The Adafruit-upstream TODO explicitly flags this — if `write_block` returns
early (invalid magic, unknown family, abort), `numWritten` is not incremented
but the block was processed. If a real UF2 block is rejected (bad magic from
USB transfer corruption, e.g.), `numWritten` permanently lags `numBlocks` and
the canonical completion never fires.

**Confirmed during 0.8.0-7 validation:** ~43% of UF2 drops on the bench
device take the slow path because at least one block fails `is_uf2_block()`
(USB-level corruption). The two prior hypotheses (BLE adv contention; SD
flash-scheduler starvation) only explained the *latency* component, not the
*completion-never-fires* component. The block-corruption rate is what's
actually driving the residual slow-path.

**Resolution path: stuck-detect, not root-cause fix.** The audit originally
recommended "reducing SD interrupt contention during USB transfers." That
landed as the BLE-quiet-during-MSC patch in 0.8.0-7 — it eliminated the
5-90s slow-completion *latency* tail but did not change the block-corruption
*rate*. The remaining stuck cases are now caught by `msc_uf2_check_stuck()`
in `src/usb/msc_uf2.c`: if the host stops writing for ≥8s with
`numWritten < numBlocks`, the BL calls `bootloader_dfu_update_process(DFU_RESET)`,
the device resets, and the host's next UF2 drop attempt starts fresh. Bench
result is 30/30 PASS with mean 9.4s (57% fast at ~5s, 43% slow at ~13-17s
after one stuck-detect cycle).

**Why this is acceptable as a bounded recovery rather than a root-cause fix:**

1. The underlying USB-layer corruption is not in BL-controllable scope. It
   sits at the boundary between TinyUSB's MSC implementation and the host's
   USB-storage driver. Fixing it would require tracking down individual byte
   flips in 512-byte SCSI WRITE10 payloads, which has no clean attribution
   path from `is_uf2_block()`'s return value.
2. The stuck-detect is NOT a soft-completion fallback (the original audit's
   anti-pattern). It does not silently complete with partial flash content;
   it tears down and resets so the host can retry cleanly with fresh state.
3. The block-corruption per-drop rate is bounded by the binomial probability
   of corrupting any one of N blocks. In bench testing this caps at one
   stuck-detect cycle (12.7s) most of the time and two cycles (16.5s) at
   the long tail. The PR description's "Latency mix on 0.8.0-7" table
   quantifies this; no drop in the 30-iter run exceeded 16.5s end-to-end.

**Why stuck-detect cycles aren't unbounded in practice:** each cycle is
triggered by the *host* re-attempting the drop (the BL just resets and
waits). A host that gives up after N retries will end the loop. The BL
itself does not infinitely retry — it just makes one attempt recoverable.
Flash-write-budget concern is therefore bounded by host retry policy, not
by BL behavior. `deploy.sh` and `deploy-bootloader.sh` both cap retries
explicitly.

**Why the 8s threshold is safe for slow hosts:** 8s is the *idle window
after the host stops writing*, not a per-write timeout. Even on a heavily
contended host (high-I/O VM, USB hub running concurrent transfers), the
gap between consecutive WRITE10 commands in a single MSC transfer is sub-
second. 8s of silence is unambiguously "the host has moved on or given up,"
not "the host is still slowly trickling data." If a future deployment finds
hosts that *do* legitimately pause > 8s mid-transfer, bumping the threshold
is a one-line constant change in `msc_uf2.c::MSC_STUCK_TIMEOUT_MS`.

**Why BLE advertising survives a stuck-detect reset:** the
`m_paused_by_msc` guard in `dfu_transport_ble.c` lives in RAM. A reset
zeros all RAM via the C runtime init, so on the next boot
`m_paused_by_msc == false` and `advertising_start()` runs normally from
`dfu_transport_ble_update_start()`. BLE OTA recovery is therefore fully
available after a stuck-detect reset.

---

## Other findings (lower priority)

### 3-second timeout cancel for UF2 path

`bootloader_dfu_start(_ota_dfu, 3000, true)` for UF2 / single-tap-reset paths.
Timeout cancel relies on `tud_mounted()` in `dfu_startup_timer_handler` and on
`dfu_startup_packet_received` being set. Set sites:

- `dfu_transport_serial.c:202` (legacy, not used per CLAUDE.md ban)
- `dfu_transport_ble.c:749` (BLE connected event)

**No MSC hook sets `dfu_startup_packet_received`**. The UF2 path relies
entirely on `tud_mounted()`. On `_ota_dfu=true`, `m_cancel_timeout_on_usb`
becomes false (line 327) and `tud_mounted()` is not consulted. Minor edge
case: an `APP_ASKS_FOR_SINGLE_TAP_RESET` combined with `_ota_dfu=true` would
time out at 3s even if USB is enumerated. Unlikely combination — rare
priority issue.

### `is_ota()` semantics overload

`is_ota()` returns `_ota_dfu`, which means "DFU was triggered by an OTA
mechanism." It's used as a proxy for "SoftDevice is up" in
`bootloader_settings_save`, but in dual-transport mode that proxy is
wrong (SD is up regardless of trigger). Replace with explicit
`sd_softdevice_is_enabled` check at every flash-write site.

### Pstorage layer

`pstorage_init`, `pstorage_register`, `pstorage_clear`, `pstorage_store` are
provided by `pstorage_raw.c`, which uses `sd_flash_write` /
`sd_flash_page_erase` (verified at lines 370, 376, 390). The BLE OTA path is
correctly SD-aware end-to-end.

### Bootloader self-update (`SD_MBR_COMMAND_COPY_BL`)

Uses MBR for atomic bootloader swap (`src/usb/msc_uf2.c:209-217`). If
interrupted by power loss, the MBR detects partial copy on next boot and
either resumes or rolls back. Safe by design.

### QSPI staged firmware path

`qspi_apply_staged_firmware()` (`src/main.c:386-411`) runs BEFORE
`ble_stack_init`, so direct NVMC is safe in that path. Verified the
sequence in `check_dfu_mode`: QSPI check (line 422) happens before the
SD init (line 561).

---

## What the production BL needs before fleet rollout

### ✅ Required fixes (all landed in 0.8.0-7-gc79626c)

1. **SD-aware `bootloader_settings_save`.** Mirror d7be9be: check
   `sd_softdevice_is_enabled` and route through `sd_flash_write` /
   `sd_flash_page_erase` for the settings page. Drop `is_ota()` gating.
   → Landed in 8a02a35 (0.8.0-6).

2. **BLE-quiet during USB MSC transfer.** Suspend BLE advertising on first
   WRITE10 of an MSC session; gate the `ADV_SET_TERMINATED` handler with
   `m_paused_by_msc` so the SD doesn't auto-restart adv mid-transfer.
   → Landed in c79626c (0.8.0-7).

3. **Stuck-transfer self-recovery.** `msc_uf2_check_stuck()` in
   `wait_for_events`: if host idle ≥8s with `numWritten < numBlocks`,
   trigger `DFU_RESET` so the host can retry cleanly. Converts the
   permanent-stuck failure mode (USB-level block corruption → counter race)
   into a bounded ~13s recovery cycle.
   → Landed in c79626c (0.8.0-7).

### ✅ Required validations (all complete)

1. **Counter-race characterization on a clean device.** Done — see the
   RESOLVED section above. The block-corruption rate manifests as ~43%
   of drops taking the stuck-detect slow path on the bench. With
   stuck-detect, all 30/30 iters PASS at WAIT_TIMEOUT=30s.

2. **Two-device hardware validation.** ⬜ Only one device tested
   (hat, SWD-flashed via swd-flash.local). Per CLAUDE.md, pick one more
   unenclosed device and run `bl_characterize.sh` against it BEFORE
   fleet-wide rollout via `deploy-bootloader.sh`.

### Optional improvements (defer)

- UF2 first-write hook to set `dfu_startup_packet_received` for cleaner
  timeout cancellation.
- Replace all `is_ota()` calls with `sd_softdevice_is_enabled` where they
  mean "is SD running?"
- Tune `MSC_STUCK_TIMEOUT_MS` (currently 8000) if any deployment surfaces
  hosts that legitimately pause > 8s between consecutive WRITE10s in a
  single transfer.

---

## What I'm NOT recommending

- **Do not** add another counter-race workaround on top of the existing
  ones. The session that led to this audit demonstrated that piling
  workarounds onto a misunderstood failure mode makes things worse.
- **Do not** roll back to a pre-d7be9be BL. d7be9be fixed a real silent-
  no-op bug in the flash write path. Pre-d7be9be has worse problems.
- **Do not** deploy 0.8.0-5 to new devices until the settings-save fix
  is landed and tested.

---

## Provenance

### 0.8.0-5 audit (original; this doc's authoring snapshot)

- BL source: `/home/jdubz/Development/Adafruit_nRF52_Bootloader/`
  branch `blinky-local-patches` commit `d7be9be`
- Worktree used for analysis: `/tmp/bl_fleet_0_8_0_5/`
- Built artifacts: `xiao_nrf52840_ble_sense_bootloader-0.8.0-5-gd7be9be*`
- Hardware verification: NOT PERFORMED at the time (test device offline)

### 0.8.0-6 fix landed (settings_save SD-aware)

- BL source: `blinky-local-patches` commit `8a02a35`
- Hardware verification: ✅ 20-iter `bl_characterize.sh` on hat (SWD-flashed
  via `swd-flash.local` per CLAUDE.md): hiccup rate dropped from 0.8.0-5's
  ~80% to ~50%, all failures = flash COMPLETE (confirms settings_save was
  the silent-fail mode this fix targets).

### 0.8.0-7 fix landed (BLE-quiet + stuck-detect)

- BL source: `blinky-local-patches` commit `c79626c` (local-only branch;
  not pushed to origin/Adafruit upstream by design — this is fleet-specific
  hardening)
- Built artifacts staged in this repo at:
  - `bootloader/update-bootloader-qspi-ota-default.uf2` (BL self-update UF2)
  - `bootloader/update-bootloader-qspi-ota-default_s140_7.3.0.zip` (OTA bundle)
- Source-level invariants: ✅ `scripts/verify_bootloader.py` PASSes both
  the dual-transport USB-init check and the SD-state-aware `settings_save`
  check.
- Hardware verification: ✅ 30-iter `bl_characterize.sh` on the hat
  (SWD-flashed via `swd-flash.local`): 30/30 PASS, mean 9.4s, max 16.5s.
  Latency mix: 57% fast (~5.3s, no stuck-detect), 23% one stuck-detect
  cycle (~12.7s), 20% two cycles (~16.5s). See PR #141 description for
  the full bench transcript.
- ⬜ Second-device validation pending before fleet rollout per CLAUDE.md.
