# Web Updater (blinky-time.joshwentworth.com/#firmware)

Lets anyone with a blinky device update it to a **stable release** from a browser: no blinkyhost, no blinky-server, no repo checkout, one device per update. Personal devices run exactly the fleet's firmware. There is no separate build, so a personal device joins the fleet like any other when it's in range of a blinky network (see [Fleet interplay](#fleet-interplay)).

> **Status (2026-10-21): live, USB only, since PR #266.** The Firebase deploy builds with `VITE_FIRMWARE_UPDATES=on` (`.github/workflows/blinky-console-deploy.yml`), which turns on `FIRMWARE_UPDATES_ENABLED` (`blinky-console/src/MainShell.tsx`). To hide the page again, remove that env line and redeploy. Bluetooth is built but hidden (`BLE_UPDATES_ENABLED = false` in `FirmwarePage.tsx`). A person updated a device end to end (`b211-dev` → b215) on 2026-10-20. The owner turned it on with checklist item 2 open and item 4 parked; recovery over USB on Windows is still limited (see [Recovery](#recovery)). `blinkytime.joshwentworth.com` (no hyphen) redirects here.

Code:
- **Page:** `blinky-console/src/pages/FirmwarePage.tsx`.
- **Checks and protocol:** `blinky-console/src/services/firmware/` (`otaFlash.ts`, `otaClient.ts`).
- **Firmware:** `blinky-things/ota/` (`QspiOtaStaging.h`, `OtaManifest.h`, `OtaKeys.h`).
- **Signing:** `tools/ota_manifest.py`.

Why this route, and what each platform allows (iPhone, Windows UF2, Chrome's DFU blocklist): [WEB_UPDATE_RESEARCH_2026_10.md](WEB_UPDATE_RESEARCH_2026_10.md).

## How an update works: signed OTA through the running app

The update never goes through the bootloader's UF2 drive or its BLE DFU service; the page talks to the running firmware instead.

1. **Connect.** The page connects over **USB** (Web Serial). **Bluetooth** (Web Bluetooth to the app's Nordic UART service) is built and tested in code but **hidden** (`BLE_UPDATES_ENABLED` in `FirmwarePage.tsx`) until it passes on hardware; see [Open items](#open-items).
2. **Signed manifest.** The page sends `ota manifest <hex>`. The device refuses unless all of these hold:
   - the Ed25519 signature verifies against the release key compiled into it;
   - the board is ours;
   - the load address is the application start;
   - the size fits;
   - the build is **newer** than its own (a separately signed emergency-rollback flag is the only exception).
3. **Image.** It sends the image as `ota chunk <offset> <crc16> <base64>` lines of 512 bytes. They go in order, each with its own CRC, and the device hashes them (SHA-512) as they arrive into its 2 MB QSPI flash. Throughout, the old firmware keeps running, and an interrupted transfer resumes from the device's count.
4. **Commit.** `ota commit` makes the device re-read the staged image from QSPI and re-check its SHA-512. Only then does it arm the bootloader (RAM magic and `GPREGRET`) and reset.
5. **Install.** The bootloader checks the image's CRC and copies it into internal flash (about 10–15 s).
6. **Verify.** The page finds the device again by chip serial number, checks it, restores runtime settings and saves, then reports success. It watches the first 75 s of uptime in the background and speaks up only if the device restarts.

Why this route:
- **The UF2 drive can't work for a person on Windows** with bootloader 0.8.0-10. Windows writes `System Volume Information` as the drive mounts, and the bootloader's stuck-transfer check drops the drive 8.5 s later (measured on hat_v1). A bootloader fix (0.8.0-11) exists but is parked; see [Recovery](#recovery).
- **Chrome can't use the bootloader's BLE DFU.** Its Web Bluetooth GATT blocklist bans Nordic legacy DFU (`00001530-…`).
- **This route avoids both,** and it doesn't erase anything until a verified image is waiting.

Before any public page or nearby Bluetooth central could reach that path, it had to be signed. The old unsigned `ota begin/chunk/commit` are gone.

**Platforms:**

| Platform | Connection |
|---|---|
| Desktop Chrome/Edge | USB |
| Android Chrome | Not yet (needs Bluetooth) |
| iPhone/iPad | Not yet (needs Bluetooth, through the free Bluefy browser; Safari has no Web Bluetooth) |
| Firefox desktop (151+) | Web Serial behind an extra prompt (not tested) |

### Signing

- **Release key.** The private key is the secret `BLINKY_RELEASE_SIGNING_KEY` of the GitHub **environment** `release-signing`, whose branch policy allows only `master`. A repository secret would be readable from any pushed branch. It is used only by `sign-release.yml`. The maintainer keeps an offline backup; losing the key means devices can never take a signed update again. The public key is compiled into the firmware (`ota/OtaKeys.h`). `firmware_release.py sign` refuses any key whose public half differs from it.
- **Owners need no key.** Their devices carry only the public key.
- **Only gated releases are signed.** Every device installs a validly signed newer build, so the signature is what makes a build installable everywhere. Candidates are never signed. `release_promote.py` dispatches `sign-release.yml`, which refuses without a passing bench-gate record for exactly that version. It attaches `.bin` + `.otam` and updates `manifest.json`. Promotion then re-verifies the signature itself before tagging the release stable.
- **Rotation.** Ship, signed with the old key, a firmware whose `OtaKeys.h` carries the new public key.
- **Bench key.** `scripts/build.sh --dev --ota-bench-key` builds firmware that trusts the bench key (private half in `~/.blinky/ota-bench-key.pem` on the bench machine). Release builds can't carry it: `OtaKeys.h` `static_assert`s a `-dev-` version. It's for testing the path on a test device without ever signing a dev build with the release key.

### What still bypasses signing

- **USB physical access.** UF2 drag-and-drop and SWD are unsigned. That's acceptable: whoever holds the device can reflash it.
- **`bootloader ble` over NUS, then legacy BLE DFU with a native tool** (not a browser). This is pre-existing: the fleet's sealed-device recovery depends on it. Closing it needs authenticated console commands; see [Open items](#open-items).

## Where the firmware comes from

```
release.yml (candidate bN: hex, uf2; unsigned) ──► release_gate.py (bench gate on a test device)
release_promote.py bN ──► sign-release.yml (gate passed? sign: bin, otam) ──► verify ──► stable-bN, Latest ──► console redeploy
.github/workflows/blinky-console-deploy.yml
   tools/firmware_release.py mirror --out blinky-console/public/firmware --keep 5
Firebase: /firmware/index.json + /firmware/bN/blinky-things-bN.{hex,uf2,bin,otam}
```

- **Only stable releases are mirrored.** "Stable" has one definition, in `firmware_release.py`. The page offers updates only from releases that carry the signed pair; older releases appear only for recovery.
- **Redeploys.** `release_promote.py` dispatches the console deploy after every promotion and rollback. `workflow_dispatch` runs `master`'s workflow, and tag/release events would run the tagged commit's.
- **Why a mirror:** GitHub release assets can't be fetched from a browser (no CORS).
- **Content types.** Firebase labels compressed `.uf2`/`.hex` responses `text/html`. `firebase.json` sets explicit types, and the page judges every firmware file by its sha256 alone.

## What the page checks (all fail loudly; nothing falls back)

**Before touching the device:**
- sha256 of the `.bin` and `.otam` against the index.
- The manifest's layout, build (= the release's), board and load address.
- The image's SHA-512 against the manifest.
- The device then verifies the signature itself.

**Pre-flight** (`deviceGates.ts`, `flashFlow.ts`, ported from `tools/deploy_local.py`):
- `json info`: an nRF52840 with a hardware serial, and **configured**.
- Confirmations for a reinstall, a downgrade, or a `SETTINGS_VERSION` change.
- **Uptime ≥ 75 s** before anything that ends in a reset, which is also what the device enforces for `ota commit`. `deploy_local.py` uses the same 75 s for firmware that reports `resetReason` in `json info`, and 5 min 15 s for older firmware, which counts every boot for 5 minutes. Signed OTA only exists on b215+, whose reboot counter counts only fault resets (watchdog or CPU lockup), and `ota commit`'s soft reset is neither.
- `ota status` must answer. Firmware older than signed OTA gets a clear "update it once another way" message.

**After the update:**
- The same chip (sn) and device id come back, configured, on **exactly** the release version, with audio counters and the BLE stack up, all checked before `save`.
- Then `restore_runtime_settings` + `save`.
- Then success is reported straight away; no 6 s metrics wait (frame rate is only a warning here). The page keeps polling until 75 s of uptime and logs an error only if the device restarts. Unplugging early is a power-on reset, which the reboot counter doesn't count. A firmware that crashes before it starts is caught by the bootloader's handshake watchdog, so this is a report, not a gate.

**Cancel** works until `commit`. A cancelled or failed transfer leaves the old firmware running and a half-filled staging area, which the next manifest erases. While an update runs, `#firmware` stays mounted through Back and warns before the tab is closed.

## Recovery

*Device stuck in update mode* (the XIAO-SENSE drive is showing, no app to talk to) writes the release UF2 to the drive. The drive is picked through the File System Access API, or the user copies the downloaded file after the page has checked `INFO_UF2.TXT`. **This doesn't work on Windows with the fleet bootloader (0.8.0-10):** it drops the drive 8.5 s after Windows' metadata write. Recovery from macOS, Linux or ChromeOS works. The page accepts only bootloader `0.8.0-10` (`ACCEPTED_BOOTLOADER_PREFIXES` in `flashLayout.ts`).

**Bootloader 0.8.0-11 is parked by owner decision.** It starts the stuck-transfer timer only after the first real UF2 block, which would fix Windows. It is built and passes `verify_bootloader.py`, on branch `fix/uf2-stuck-check-valid-blocks-only` of the bootloader fork (draft PR Jdubz/Adafruit_nRF52_Bootloader#1). It is not flashed anywhere. If it is ever picked up, it goes to a sacrificial device first, through `deploy-bootloader.sh`.

With 0.8.0-11 the drive would also stop timing out back to the app when nothing is copied. It would wait for a file, a reset or an unplug, as upstream Adafruit does. CLAUDE.md and `deploy_local.py`'s messages would have to change with that rollout.

## Bench results (hat_v1, 2026-10-20)

- **Root cause found and fixed:** the QSPI flash's Quad Enable bit was off. The app and the bootloader both use quad reads and writes, and with QE=0 every staged image read back wrong:
  - `ota selftest`: CRC mismatch;
  - the first signed update: `ota commit` → `readback-mismatch`. The device refused, and its old firmware kept running, as designed.

  `QspiOtaStaging::begin()` now sets QE once (`SR2 0x0 → 0x2`, non-volatile) and fails loudly if it can't. The self-test then passed.
- **Firmware refusals on hardware:** release-signed manifest on a bench-key build → `bad-signature`; older build → `not-newer`; garbage → `bad-magic`; commit with nothing staged → `not-complete`.
- **Full USB update in a real browser (Chromium, Web Serial):** `b211-dev` → `b212-dev-f10c4772`.
  - The 484,764-byte image transferred in ~20 s (~24 KB/s).
  - The device verified it, the bootloader installed it, and the app was back in ~8 s.
  - Exact-version, identity, config and BLE checks passed, then restore + save and the 75 s hold. Confirmed afterwards over serial.
  - The only automation stand-in: Chrome's serial-port chooser was answered with the already-permitted port.
- **Full USB update from a real signed release, by a person on the live mirror:** `b211-dev-15db1bda` (current master, trusting the release key) → **b215**, the first release signed by `sign-release.yml`. Served from a Firebase preview channel of the console with updates on, using the same mirror the live site serves (the `.bin` and `.otam` hashes match the GitHub release). The owner clicked it through in Chrome. Afterwards over serial: `b215-39df3912`, `hat_v1`, configured, 120 fps.
- **Bluetooth: not working from Windows Chrome yet.**
  - The hat connected, but every GATT operation took ~5 s and a 10-byte write failed after ~10 s (`GATT operation failed for unknown reason`).
  - A native client (`bleak`) on the same PC saw an incomplete service table.
  - The hat restarted on its own shortly afterwards. **Cause found and fixed (#173):** a Windows central negotiates MTU 527, and the NUS send path copied up to 524 bytes into a 244-byte stack buffer, so any reply longer than ~244 bytes smashed the stack and soft-reset the device.
  - This is the app's existing BLE/NUS stack, not the update protocol, which stays safe over a bad link because nothing is installed until a verified image is complete. See [Open items](#open-items).

## Rollout checklist

The page went live (USB) on 2026-10-21 (PR #266), after item 1 and a real end-to-end update by a person (bench results). What's still open:

1. ✅ Done (b215). Merge; cut a candidate (`release.yml`); gate it on hat_v1 (`release_gate.py`); promote it (`release_promote.py`, which has `sign-release.yml` sign it). The mirror then carries the first signed release.
2. **Open.** Get that release onto devices once by the existing routes (`deploy.sh` for the fleet, `deploy_local.py` for USB devices). Firmware before it can't do signed OTA, and the page says so.
3. Bluetooth (optional for a USB-only launch; it's hidden): fix and verify on **Android Chrome** (the main Bluetooth target) and desktop Chrome, including a dropped link mid-transfer (resume) and timing. Then set `BLE_UPDATES_ENABLED`.
4. **Parked (owner decision); recovery over USB on Windows stays limited.** The page's recovery option (a device stuck as the `XIAO-SENSE` drive) uses the bootloader's UF2 drive, which 0.8.0-10 drops on Windows (`msc_uf2_check_stuck`). Recovery from macOS, Linux or ChromeOS is unaffected. The fix is bootloader 0.8.0-11 (draft fork PR #1, not flashed). If it is picked up: sacrificial device first, through `deploy-bootloader.sh`. In the same change: add `0.8.0-11` to `deploy_local.py --expect-bl` **and** the page's `ACCEPTED_BOOTLOADER_PREFIXES` (Python first), and update CLAUDE.md / `deploy_local.py` for its no-timeout behaviour.
5. iPhone (after Bluetooth): one full update in Bluefy.

## Open items

- **Bluetooth from Windows Chrome:** slow GATT operations and a failed write (bench results). The self-restart was #173 (fixed: MTU-sized stack overflow on long replies); NUS commands now also run on the loop task. Still to do: reproduce a full update on Android Chrome, and check the scanner duty cycle (`BleScanner`, window 144 of 160) against connection events.
- **Authenticated console commands:** `bootloader ble`, `device upload`, `wipe_device_identity` etc. are accepted from any NUS central or broadcaster. Signing the update path doesn't cover them.
- **Power loss during the bootloader's copy:** the apply request is one-shot (RAM/`GPREGRET`). A power cut mid-copy leaves the device in its bootloader, which is recoverable but not self-healing. Fix: a persistent "copy pending" marker in the bootloader.
- **Hub-driven updates** (blinkyhost offering the stable release to personal devices in range) are deferred by decision.

## Fleet interplay

- **A personal device joins on its own.** There is no enrollment or network ID: the firmware obeys any in-range broadcaster, and blinky-server adopts any `Blinky-*` advertiser as `PRESENT`.
- **Fleet deploys reach personal devices.** `./scripts/deploy.sh --devices=all` includes every in-range nRF52840, personal ones too. Both routes install only stable releases.
