_Created: 2026-05-13_

# Sculpture BLE-Recovery Plan

## Goal

Make installed sculptures **fully recoverable via BLE** with **zero physical access required**. After install, the reset button, USB port, and SWD pads are inaccessible (sealed enclosure, indoor/outdoor mount). The only operator path is the Pi fleet server over BLE.

Constraint: a brick after install is a permanent loss — the sculpture either gets cut open or thrown away.

## Threat model

A sculpture is "bricked" if all of these are simultaneously true:
- App is unresponsive or crash-looping, AND
- BLE NUS in the app is not reachable (too short an uptime window, BLE init failed, or app jumped to a HardFault before BLE came up), AND
- The Adafruit bootloader is not in (or did not stay in) BLE DFU mode.

We need at least one *autonomous* path — no human button presses — that gets the device into bootloader BLE DFU mode and keeps it there until the fleet server can push new firmware.

## Current state (audit findings, 2026-05-13)

The recovery system today has three layers:

1. `SafeBootWatchdog::begin()` (`hal/SafeBootWatchdog.h:191`) — first call in `setup()`. GPREGRET2 boot counter; at `≥ 3` writes RAM magic `0xBEEF00A8` and resets → bootloader BLE DFU.
2. `SafeMode::check()` (`tests/SafeMode.h:124`) — `.noinit` RAM counter; at `> 3` enters an infinite USB-CDC + LED-blink `while(true)` loop.
3. Adafruit bootloader (`Adafruit_nRF52_Bootloader/src/main.c:413`) — RAM magic + GPREGRET + button + app-validity check; advertises BLE DFU forever (`bootloader_dfu_start(_ota_dfu, 0, false)`) when entered via OTA magic.

### Risks identified

| # | Risk | Where | Severity |
|---|---|---|---|
| **R1** | `SafeMode::enterSafeMode()` is a permanent USB-only trap with no BLE | `tests/SafeMode.h:57-118` | 🛑 Blocker |
| **R2** | `markStable()` defeats 3-strikes for any crash that happens *after* `setup()` completes | `hal/SafeBootWatchdog.h:213`, `.ino:494` | 🛑 Blocker |
| **R3** | Bootloader does NOT default to BLE DFU when app is invalid — sits in USB UF2 mode forever | `Adafruit_nRF52_Bootloader/src/main.c:489-555`, `DEFAULT_TO_OTA_DFU` not defined | 🛑 Blocker |
| **R4** | GPREGRET2 + `.noinit` RAM both wipe on power-on reset — any power blip resets the 3-strikes counter | `SafeBootWatchdog.h:24` (hardware) | 🟠 High |
| **R5** | BLE DFU entry has no GPREGRET fallback (UF2 path has both RAM and GPREGRET) | `SafeBootWatchdog.h:108-110` | 🟠 High |
| **R6** | `bleDfu.begin()` return value ignored — silent failure leaves device with no DFU service | `.ino:420` | 🟡 Medium |

### Already safe (verified)

- BLE NUS advertises forever in the app (`BleNus.cpp:40`).
- Bootloader BLE DFU advertises forever via OTA RAM magic (`main.c:555`, timeout=0).
- `bootloader ble` command available on USB serial AND BLE NUS (`SerialConsole.cpp:831`).
- 15 s hardware WDT catches main-loop hangs (`SafeBootWatchdog.h:165-173`).
- QSPI staged OTA validates-before-commit; aborted copy leaves old app intact (`main.c:262-433`).
- Fleet server already auto-discovers bootloader-mode devices via DFU service UUID `0x1530`.
- Bootloader BLE address = app address + 1 (stable, documented per device).

## Required fixes (priority order)

### F1 — Rebuild bootloader with `DEFAULT_TO_OTA_DFU` (closes R3) — P0

**Change:** Add `target_compile_definitions(bootloader PUBLIC ... DEFAULT_TO_OTA_DFU)` for the `xiao_nrf52840_ble_sense` board build (or unconditionally in `CMakeLists.txt`).

**Effect:** With this flag, an invalid app *or* any DFU entry that doesn't specify UF2/serial defaults to BLE DFU (`main.c:489-494`). Mid-DFU interruption, QSPI commit failure, vector-table corruption — all roads lead to BLE DFU advertising forever instead of USB UF2 mode forever.

**Deploy:** SWD-flash via Pi GPIO (`openocd -f interface/bcm2835gpio-swd.cfg -f target/nrf52.cfg`) to every bench unit first, validate, then to sculpture units.

**Verify:** SWD-erase the app region on a bench unit, power-cycle, confirm device enters BLE DFU and fleet server can connect to bootloader address and reflash.

### F2 — Remove or rewrite `SafeMode::enterSafeMode()` (closes R1) — P0

**Options:**
- **Preferred:** Delete `tests/SafeMode.h` and its call site at `.ino:151`. `SafeBootWatchdog` supersedes it.
- **Alternative:** Replace `enterSafeMode()` body with `SafeBootWatchdog::enterBleDfuBootloader()`.

**Effect:** No code path can drop the device into a USB-only infinite loop after install.

**Verify:** Force `SafeMode::forceSafeMode()` on a bench unit before applying the fix, confirm brick. After fix, confirm BLE DFU instead.

### F3 — Defer `markStable()` until ≥ 60 s of stable uptime (partially closes R2) — P0

**Change:** Remove `SafeBootWatchdog::markStable()` from end of `setup()` (`.ino:494`). Move it into `loop()` behind a one-shot check: if `millis() > 60'000` and `!stableMarked_`, call `markStable()` once.

**Effect:** A crash 5 s into normal operation no longer clears the counter. The counter advances on each fast-fail boot until it hits 3 → BLE DFU recovery.

**Trade-off:** First 60 s of every boot the counter is non-zero. A user-initiated reboot followed by 3 power blips within 60 s of each boot could trigger spurious recovery. Acceptable — recovery only delays the device by one BLE DFU cycle (~5.5 min) and is otherwise harmless.

### F4 — Flash-backed reboot-frequency counter (closes R2 + R4) — P0

**Change:** Add a new module `hal/RebootFrequencyCounter` that:
- Stores a counter and a "last boot timestamp" in a dedicated 4 KB flash sector (separate from `ConfigStorage` to avoid corruption coupling).
- On boot, increments the counter. If the new value ≥ N (suggest 5), enter BLE DFU recovery.
- After stable uptime ≥ 5 minutes, clears the counter.
- Uses simple CRC-protected record so corruption reads as "0" rather than triggering spurious recovery.

**Why separate from F3:** GPREGRET2 wipes on POR (`SafeBootWatchdog.h:24`). A sculpture with intermittent power and a slow-crashing app would otherwise defeat the 3-strikes mechanism by power-cycling the counter to 0 between crashes. Flash survives.

**Verify:** Power-cycle a crashing unit repeatedly; confirm BLE DFU triggers on the 5th boot regardless of power cycles between.

### F5 — Add `GPREGRET = 0xA8` fallback in BLE DFU path (closes R5) — P0

**Change:** One-line addition in `SafeBootWatchdog::enterBootloaderWithMagic(0xA8)` (`hal/SafeBootWatchdog.h:108`):
```cpp
} else if (magic == 0xA8) {
    *bootloader_ram = 0xBEEF00A8;
    NRF_POWER->GPREGRET = 0xA8;  // DFU_MAGIC_OTA_RESET — stock bootloader fallback
}
```

**Effect:** Matches the UF2 path's belt-and-suspenders. If RAM is clobbered between reset and bootloader (exotic but documented USB-hub case), GPREGRET still triggers BLE DFU on the stock bootloader.

### F6 — Check `bleDfu.begin()` return (closes R6) — P1

**Change:** In `.ino:420`, capture the return value, log error on failure, possibly halt+reset.

**Effect:** Silent BLEDfu registration failure becomes loud (`[FALLBACK]` per CLAUDE.md no-silent-fallbacks rule), so fleet can detect and re-flash before sealing.

## Verification matrix (must pass on every unit before install)

| # | Test | Validates |
|---|---|---|
| **T1** | SWD-erase app, power-cycle, confirm BLE DFU advertising autonomously | F1 |
| **T2** | Flash image that crashes in `setup()`, confirm 3-strikes triggers BLE DFU, fleet recovers | F3 |
| **T3** | Flash image that runs 10 s then `__BKPT(0)`, confirm F4 counter triggers BLE DFU after 5 boots | F4 |
| **T4** | T3 + power-cycle between every crash; confirm F4 still triggers | F4, R4 |
| **T5** | Start BLE DFU upload, drop BLE partway, power-cycle, confirm device enters BLE DFU on next boot | F1 |
| **T6** | Force `SafeMode::forceSafeMode()` (pre-F2) — confirm brick. Apply F2, retry — confirm BLE DFU. | F2 |
| **T7** | QSPI `ota commit` with intentionally bad CRC — confirm old app boots normally | (existing path) |
| **T8** | End-to-end fleet flash via BLE DFU — confirm full 510 KB transfer succeeds and device auto-reconnects | (existing path) |

## Out of scope (for now)

- **BLE-bondless DFU**: bootloader already does this (no bond required). Confirmed.
- **Secure DFU / signed firmware**: Adafruit bootloader uses Legacy DFU (SDK v11). Adding signing would require migrating to Secure DFU (SDK v15+) — a much larger project. Not blocking sculpture install.
- **Periodic "dead-man" recovery** (device voluntarily enters BLE DFU after N hours of no fleet contact): considered, deferred — F4 covers crash-loops; the dead-man case requires the app to be running well enough to count time, in which case BLE NUS works anyway.
- **Secondary radio / LoRa fallback**: not in scope. Pi server is line-of-sight BLE.

## Community research (2026-05-13)

### Validated: `DEFAULT_TO_OTA_DFU` is the established fix

The community fork [`oltaco/Adafruit_nRF52_Bootloader_OTAFIX`](https://github.com/oltaco/Adafruit_nRF52_Bootloader_OTAFIX) was created specifically to handle the "sealed BLE-only device" case. From their README:

> When no valid application is present, the bootloader defaults to OTA DFU mode. This prevents devices from becoming stuck in UF2 mode after a failed OTA update.

Adafruit issue [#259 "AdaDFU does not re-appear after a failed OTA"](https://github.com/adafruit/Adafruit_nRF52_Bootloader/issues/259) (Jan 2022, still open) confirms this is **the** known bricking pattern for sealed devices. There is no fix in stock Adafruit. F1 is correct.

### OTAFIX has additional improvements worth adopting

Beyond `DEFAULT_TO_OTA_DFU`, OTAFIX 2.1/2.2 adds several reliability improvements that *reduce DFU failure rate* (not just improve recovery):

- **High-MTU BLE support** — larger DFU packets, faster transfers, smaller failure window.
- **Lazy flash erase** — flash pages erased on-demand during transfer instead of upfront. Removes the ~25 s START_DFU stall window that disconnects often happen in.
- **Small-packet accumulation** — combine sub-64-byte packets and write 240-byte chunks; faster, fewer flash ops.
- **Auto-boot after successful USB OTA** — no manual reset needed.
- **Board-specific BLE names** (`TNM1_DFU`, etc. instead of `AdaDFU`) — easier fleet identification.
- **BLE TX power +8 dBm in bootloader** — better range, fewer connection drops.

**Action:** Evaluate adopting OTAFIX as upstream rather than maintaining our own custom fork. Their `master` branch already contains our F1 + several improvements we'd otherwise have to write ourselves. If we adopt OTAFIX, we still need to *also* port our RAM-magic entry points (`DFU_RAM_MAGIC_UF2/BLE/QSPI`), since that solves a different problem (USB-hub power-cycling clearing GPREGRET on Windows).

### Industry validation: BLE-only sealed device pattern is NOT proven at scale

- [Meshtastic](https://meshtastic.org/docs/getting-started/flashing-firmware/nrf52/ota/) explicitly warns: *"if the update process fails, your device will be left in a non-working state and require the Drag and Drop actions for recover the firmware"* — i.e. physical USB access. Their guidance for unreachable nodes is to "run stable" channels, not to harden the recovery path.
- [Particle](https://community.particle.io/t/bricked-boron-after-using-nrf-sdk/52056) community reports: sealed bricked Boron units have been observed to "dump power and warm up continuously and cannot be stopped" without J-Link access. Particle's commercial answer is to never seal a unit that can't be J-Link recovered.
- Migration to MCUboot ([Nordic nRF Connect SDK / Zephyr](https://docs.nordicsemi.com/bundle/ncs-latest/page/mcuboot/readme-ncs.html)) provides Serial Recovery over UART/USB but does not fundamentally solve BLE-only recovery; it would be a multi-week port that doesn't change the core constraint.

**Conclusion:** *No proven public-domain pattern exists for fully reliable BLE-only recovery on sealed nRF52 devices.* The combination we're building (OTAFIX + flash-backed reboot counter + deferred markStable + multiple bootloader entry RAM magics) is novel. The community wisdom is "don't seal without a recovery path." Since our project requires sealing, we are accepting genuinely non-trivial risk that cannot be fully eliminated — only mitigated.

### New risk identified by research: DFU transport inactivity timeout

[Nordic DevZone](https://devzone.nordicsemi.com/f/nordic-q-a/51352/nrfutil-does-not-disconnect-after-ble-dfu-error): even when `bootloader_dfu_start(_, 0, _)` is called (no startup timeout), the DFU *transport* has an internal inactivity timer that resets the device back to the app after a failed session. If the app is valid (just buggy), the device exits DFU mode and resumes crash-looping. Mitigation: F4's flash-backed counter re-triggers BLE DFU on the next boot — so the fleet server gets multiple recovery windows, just not a single continuous one.

### Flash-backed reboot counter: no off-the-shelf nRF52 implementation found

[Memfault's watchdog article](https://interrupt.memfault.com/blog/firmware-watchdog-best-practices) covers reset-reason detection and software-watchdog patterns but **not** crash-loop counters across power cycles. We will need to roll F4 ourselves. Suggested storage: a dedicated 4 KB flash sector with a simple CRC-protected 16-byte record (counter + last-boot-timestamp + magic + CRC). Treat any read failure as "counter = 0" to avoid spurious recovery on corruption.

### Open questions still worth research before implementation

- Does OTAFIX's lazy-flash-erase change interact badly with our QSPI staged-OTA bootloader patches? (Both touch flash subsystem during DFU.)
- POWER->POFCON brownout config — at what voltage do nRF52 flash writes corrupt? Should we abort QSPI commits below a threshold?
- Does the bootloader's BLE advertising MAC remain stable after a SoftDevice-disable/re-enable cycle? (Relevant if a DFU session fails mid-transfer.)

## Sequencing

1. **Evaluate OTAFIX adoption.** Diff OTAFIX `master` against our custom bootloader. If their changes are a superset of ours plus useful extras, rebase our patches on top of theirs. If conflicts are minor, this is preferred. (1 day investigation.)
2. Land F1 (`DEFAULT_TO_OTA_DFU`) — either via OTAFIX or directly. SWD-flash to bench unit; run T1, T5.
3. Land F5 + F6 (low-risk firmware one-liners) on bench unit; rebuild app.
4. Land F2 (delete SafeMode) on bench unit; run T6.
5. Land F3 (deferred markStable) on bench unit; run T2.
6. Implement F4 (flash-backed reboot-frequency counter) on bench unit; run T3, T4.
7. Full test matrix T1–T8 on each sculpture unit before sealing.
8. Document per-device BLE addresses (app + bootloader = app+1) in a fleet manifest.

## Residual risk (after all fixes land)

Even with everything above, the following scenarios still brick a sealed sculpture:

- **Power-glitch during bootloader self-update.** No mitigation — never update the bootloader on a sealed unit. Lock bootloader version at install time.
- **SoftDevice flash region corruption** (e.g. cosmic ray hitting RAM during DFU write). Mitigation: pre-install validation of SoftDevice + bootloader integrity. Long-tail risk.
- **Bootloader BLE stack crash inside DFU session** (rare; nRF52 hardware errata). No software mitigation. Pre-install soak test would catch most cases.
- **Hardware failure** (flash wear, antenna damage, regulator failure). No software mitigation. Accept and replace via warranty.

These are the irreducible risks of a fully sealed BLE-only device. They are *low probability* but *high impact* (one brick = one sculpture lost). The plan above is the best reasonable mitigation; perfect safety is not achievable.
