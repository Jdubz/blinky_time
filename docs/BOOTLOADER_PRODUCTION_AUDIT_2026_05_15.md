# Bootloader Production Audit — 0.8.0-5-gd7be9be

**Date:** 2026-05-15  
**Scope:** Adafruit_nRF52_Bootloader at commit `d7be9be` (the fleet-deployed BL)  
**Target:** XIAO nRF52840 Sense  
**Bottom line:** **NOT production-ready.** One confirmed latent bug + one open question.

---

## Method

Source-level review with line-numbered citations. No hardware testing — the only
test device (hat) is offline awaiting SWD recovery. Conclusions are limited to
what static analysis can establish; runtime behavior must be confirmed on a
recoverable device before fleet rollout.

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

## OPEN — `writtenMask` counter race

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

**Status:** observed under stress during today's session, but the test
conditions were contaminated by my modifications. I cannot cleanly attribute
the counter race to 0.8.0-5 vs my soft-completion changes. Needs a clean test:
flash 0.8.0-5 to a fresh device, run 10–20 UF2 drops in series, count
hiccups.

**Hypothesis (not proven):** SoftDevice interrupt activity during BLE
advertising delays USB MSC servicing enough to cause occasional packet
corruption at the host's MSC layer. Corrupted blocks fail `is_uf2_block`
magic check, `write_block` returns -1, `numWritten` doesn't increment.

If confirmed, the right fix is not a soft-completion fallback (which masks
the symptom) — it's reducing SD interrupt contention during USB transfers
(e.g., suspending BLE advertising while USB MSC is mid-transfer).

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

### Required fixes (block deployment)

1. **SD-aware `bootloader_settings_save`.** Mirror d7be9be: check
   `sd_softdevice_is_enabled` and route through `sd_flash_write` /
   `sd_flash_page_erase` for the settings page. Drop `is_ota()` gating.

### Required validations (must complete before deployment)

1. **Counter race characterization on a fresh device.** Flash 0.8.0-5 to a
   pristine XIAO. Run 20 consecutive UF2 flashes. Count how many require
   re-drop. If non-zero, investigate root cause (do not paper over with
   soft-completion).

2. **The fix for #1 above must be tested on at least 2 devices** (one
   accessible, one sealed-style) before fleet rollout. Test both the BLE
   OTA path AND the UF2 path for each fix.

### Optional improvements (defer)

- UF2 first-write hook to set `dfu_startup_packet_received` for cleaner
  timeout cancellation.
- Replace all `is_ota()` calls with `sd_softdevice_is_enabled` where they
  mean "is SD running?"

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

- BL source: `/home/jdubz/Development/Adafruit_nRF52_Bootloader/`
  branch `blinky-local-patches` commit `d7be9be`
- Worktree used for analysis: `/tmp/bl_fleet_0_8_0_5/`
- Built artifacts: `xiao_nrf52840_ble_sense_bootloader-0.8.0-5-gd7be9be*`
- Hardware verification: **NOT PERFORMED** (test device offline)
