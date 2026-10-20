# Web Flasher (blinkytime.joshwentworth.com/#firmware)

Lets anyone with a blinky device update it to a **stable release** from a browser: no blinkyhost, no blinky-server, no repo checkout. One device per update, **over USB**. Personal devices run exactly the fleet's firmware. There is no separate build, so a personal device joins the fleet like any other when it's in range of a blinky network (see [Fleet interplay](#fleet-interplay)).

Code: `blinky-console/src/services/firmware/` (all checks) and `blinky-console/src/pages/FirmwarePage.tsx` (UI).

**Supported:** Chrome or Edge on a computer, with the device plugged in by USB. That needs Web Serial, plus the File System Access API for the automatic drive write.

**Not supported:**
- **Phones and tablets, Safari, Firefox.** The page says so.
- **Bluetooth updates, by decision.** The bootloader's only BLE update service is Nordic legacy DFU (`00001530-1212-efde-1523-785feabcd123`), and Chrome's [Web Bluetooth GATT blocklist](https://github.com/WebBluetoothCG/registries/blob/master/gatt_blocklist.txt) blocks it outright ("firmware update services that don't check the update's signature"). A browser can never open it. The fleet's BLE DFU (`ble_dfu.py`, a native BlueZ client) is unaffected.

## Where the firmware comes from

```
release_promote.py bN ──► GitHub Release bN (stable, tag stable-bN)
                              │  release / tag events
                              ▼
.github/workflows/blinky-console-deploy.yml
   tools/firmware_release.py mirror --out blinky-console/public/firmware --keep 5
                              │  same download() + sha256 checks deploy.sh uses
                              ▼
Firebase: /firmware/index.json + /firmware/bN/blinky-things-bN.{hex,uf2}
```

- **Only stable releases are mirrored.** "Stable" has one definition, `firmware_release.py` (not a pre-release, `stable-bN` tag present). Candidates and dev builds never reach this page.
- **The newest 5 stable releases** are offered, with the release GitHub marks *Latest* as the default. A rollback (`release_promote.py --rollback`) changes the default on the next deploy.
- **The redeploy triggers** are a `stable-b*` tag push and the release edit that follows it (promotion; `release_promote.py` tags first, so no deploy ever sees a Latest release without its stable tag), `release: edited/deleted` (rollback, deletion) and console changes. Release and tag events build the live site from `master`, not from the tagged commit.
- **No stable release yet** means `latest: null`, and the page refuses to update anything.
- **Why a mirror:** GitHub release assets can't be fetched from a browser (no CORS).
- **The page installs only the `.uf2`.** The mirror also carries the `.hex`, because it's part of the release contract the deploy tools use.
- **Content types:** Firebase labels compressed responses for unknown extensions (`.uf2`, `.hex`) `text/html`, even though the bytes are correct. `firebase.json` sets explicit types, and the page judges firmware files by their sha256 alone. Only `index.json` is checked for "the host served the app page instead".

## What the page checks (all fail loudly; nothing falls back)

**Before touching the device** (`releases.ts`, `uf2.ts`):
- sha256 of the `.uf2` against the release manifest, as copied into `index.json`.
- Every UF2 block:
  - nRF52840 *application* family (`0xADA52840`);
  - consecutive block numbers;
  - every write inside `[0x27000, 0xE6000)`, so never the MBR, SoftDevice, InternalFS `/config.bin`, bootloader or UICR. This is stricter than `uf2_upload.validate_uf2`, and tested against a real `uf2conv.py` image (`__fixtures__/make_fixtures.py`).

**Pre-flight** (`deviceGates.ts`, `flashFlow.ts`, ported from `tools/deploy_local.py`):
- `json info`: platform nrf52840, a hardware serial, and **configured** (an unconfigured device is refused; a firmware update doesn't fix that).
- The operator must confirm a reinstall, a downgrade, or a `SETTINGS_VERSION` change. The last one resets saved tuning; it is also assumed when the running version isn't a published release.
- **Uptime ≥ 5 min 15 s before the bootloader reset.** The page waits and polls, and refuses if the device restarts meanwhile. This covers the b190-era reboot counter and the 60-second rule.

**Before writing:** the drive's `INFO_UF2.TXT` must say `Board-ID: nRF52840-SeeedXiaoSense-v1` and bootloader `0.8.0-10*`, or nothing is written. The manual-copy path asks for that file through a file picker and checks it before the `.uf2` is handed out.

**After the update** (deploy_local.py steps 7–9):
- The same chip (sn) and the same device id come back, configured, on **exactly** the release version, with audio counters present and the firmware's BLE stack up (an empty `ble` means the next flash write would hang). All of this is checked before `save`.
- Then `restore_runtime_settings` + `save`.
- Then a hold until 75 s of uptime with no restart (markStable).

## The USB flow

1. **Talk to the app.** Web Serial, with the picker filtered to `2886:8045`.
2. **Enter the bootloader.** The `bootloader` command switches the device to UF2 mass storage.
3. **Write.** The page writes the UF2 onto the `XIAO-SENSE` drive through the File System Access API. Or the user copies the downloaded file there; the page first checks `INFO_UF2.TXT`, then hands out the file.
4. **Find the device again.**
   - On an update, the page reopens an already-permitted app port, but only one that answers with the same serial number.
   - On a recovery there is no serial to compare against, so the user always picks the device.

**Cancel** is honoured until the write starts. Each irreversible step re-checks it with no `await` in between.

**The old app is intact until the first UF2 block lands**, because the bootloader writes each block as it arrives.

**If the write is interrupted**, the app may be partial. The bootloader's stuck-transfer timeout (`msc_uf2_check_stuck`) resets the device. If the partial app can't boot, the app-handshake watchdog sends it back to the bootloader after 3 tries. Then *stuck in update mode* finishes the job.

An update in progress keeps `#firmware` mounted through hash navigation (Back), and warns before the tab is closed once the write is committed.

### Known limits (vs. deploy_local.py)

- **Drive identity:** a browser can't read the drive's USB serial, so the drive isn't pinned to the chip before writing. Mitigations:
  - the `INFO_UF2.TXT` board/bootloader check;
  - the post-flash serial-number check.

  Writing to another XIAO installs the same stable release there and fails verification; the user's own device stays in its bootloader with the old app.
- **No server-side audit log:** the page shows a copyable log instead.

## Bench gate: outstanding before announcing the page

The page's checks are unit-tested, and the real `b214` release passes them.

**Done, 2026-10-20, on hat_v1 (`b213` → `b214-7a4664d6`, 97 s end to end).** The page's own `flashOverUsb` ran against the device, with Node stand-ins (`serialport`, and the mounted `XIAO-SENSE` volume) for the two browser pickers. It covered:
- the gates, including the settings-reset confirmation for an unpublished running version;
- the `bootloader` entry;
- the real `INFO_UF2.TXT` check;
- the UF2 write;
- finding the device again by serial number (19 s);
- the exact-version and BLE checks;
- `restore_runtime_settings` + `save`;
- the 75 s hold.

The device came back configured, with BLE up.

That run doesn't cover the browser APIs themselves; the items below still need a real browser. Run them on a test device listed in `devices/test_devices.json`, with USB attached so `deploy_local.py --already-in-bootloader` stays available as a fallback. A device's uptime must be ≥ 5 min 15 s between runs.

- [ ] **File System Access:** Chrome/Edge on Windows, macOS and ChromeOS let `showDirectoryPicker` open the `XIAO-SENSE` drive root. Chrome writes via a `.crswap` swap file and renames on `close()`; confirm the bootloader flashes from the swap-file blocks and that the torn `close()` is reported as expected.
- [ ] **DTR:** the page asserts DTR/RTS after opening the port. The firmware's CDC layer drops all output while DTR is low, on every build. Confirm `json info` answers in Chrome and Edge.
- [ ] **Re-enumeration:** after the update, `navigator.serial.getPorts()` returns the app port without a second picker. Otherwise the page falls back to asking; note which happens.
- [ ] **Manual copy:** the `INFO_UF2.TXT` check, then download + drag, completes and verifies.
- [ ] **Interruption:** unplug mid-write. Confirm that the device ends up back in its bootloader (stuck-transfer timeout, or the handshake watchdog after 3 boots) and that *stuck in update mode* completes the update.
- [ ] **Refusals on hardware:** an unconfigured device; a device up less than 5 min (the page waits); a downgrade and a settings change (the page asks).
- [ ] **Mirror:** promote a release and check that the console redeploys with it as the default; roll back and check that the default follows.

## Fleet interplay

- **A personal device joins on its own.** There is no enrollment or network ID: the firmware obeys any in-range broadcaster, and blinky-server adopts any `Blinky-*` advertiser as `PRESENT`.
- **Fleet deploys reach personal devices.** `./scripts/deploy.sh --devices=all` includes every in-range nRF52840, personal ones too. Fleet and the web flasher both install only stable releases, so the worst case is a personal device being moved to the latest stable.
- **Pinned personal devices get moved.** A personal device pinned to an older stable release on purpose is moved to the latest stable by the next fleet-wide deploy while it's in range.
