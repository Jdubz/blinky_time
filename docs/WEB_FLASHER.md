# Web Flasher (blinkytime.joshwentworth.com/#firmware)

Lets anyone with a blinky device update it to a **stable release** from a browser: no blinkyhost, no blinky-server, no repo checkout. One device per update, over USB or Bluetooth. Personal devices run exactly the fleet's firmware. There is no separate build, so a personal device joins the fleet like any other when it's in range of a blinky network (see [Fleet interplay](#fleet-interplay)).

Code: `blinky-console/src/services/firmware/` (all checks) and `blinky-console/src/pages/FirmwarePage.tsx` (UI).

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
- **The redeploy triggers** are a `stable-b*` tag push (promotion), `release: released/edited/deleted` (rollback, deletion) and console changes. Release and tag events build the live site from `master`, not from the tagged commit.
- **No stable release yet** means `latest: null`, and the page refuses to update anything.
- **Why a mirror:** GitHub release assets can't be fetched from a browser (no CORS).

## What the page checks (all fail loudly; nothing falls back)

**Before touching the device** (`releases.ts`, `uf2.ts`, `intelHex.ts`, `legacyDfuPackage.ts`):
- sha256 of the `.uf2` and `.hex` against the release manifest, as copied into `index.json`.
- Strict Intel HEX parse: checksums, EOF present, no overlaps, no data after EOF.
- UF2 blocks are all checked:
  - nRF52840 *application* family (`0xADA52840`);
  - consecutive block numbers;
  - every write inside `[0x27000, 0xE6000)`, so never the MBR, SoftDevice, InternalFS `/config.bin`, bootloader or UICR. This is stricter than `uf2_upload.validate_uf2`.
- **UF2 == hex byte for byte.** The USB path flashes the `.uf2` and the BLE path the `.hex`; they must be the same firmware.
- The BLE DFU package is built from the hex exactly like `blinky_server/firmware/compile.py generate_dfu_package` does. Golden-tested against that code (`__fixtures__/make_fixtures.py`).

**Pre-flight** (`deviceGates.ts`, `flashFlow.ts`, ported from `tools/deploy_local.py`):
- `json info`: platform nrf52840, a hardware serial, and **configured** (an unconfigured device is refused; a firmware update doesn't fix that).
- The operator must confirm a reinstall, a downgrade, or a `SETTINGS_VERSION` change. The last one resets saved tuning; it is also assumed when the running version isn't a published release.
- **Uptime ≥ 5 min 15 s before the bootloader reset.** The page waits and polls, and refuses if the device restarts meanwhile. This covers the b190-era reboot counter and the 60-second rule.

**USB only:** the drive's `INFO_UF2.TXT` must say `Board-ID: nRF52840-SeeedXiaoSense-v1` and bootloader `0.8.0-10*`, or nothing is written.

**After the update** (deploy_local.py steps 7–9):
- The same chip (sn) and the same device id come back, configured, on **exactly** the release version, with audio counters present and **BLE up**. All of this is checked before `save`.
- Then `restore_runtime_settings` + `save`.
- Then a hold until 75 s of uptime with no restart (markStable).

## Transports

| | USB (Chrome/Edge desktop) | Bluetooth (Android Chrome, Chrome/Edge desktop) |
|---|---|---|
| Talk to the app | Web Serial, picker filtered to `2886:8045` | Web Bluetooth NUS (`Blinky*`) |
| Enter bootloader | `bootloader` (UF2 mass storage) | `bootloader ble` (legacy OTA DFU) |
| Write | UF2 onto the `XIAO-SENSE` drive via the File System Access API, or the user copies the downloaded file | Nordic legacy DFU to `AdaDFU`; 20-byte packets, PRN every 8 with the received-byte count checked |
| Old app intact until | the first UF2 block lands (each block is written as it arrives) | START_DFU (erases it) |
| Interrupted mid-write | Partial app. The bootloader's stuck-transfer timeout (`msc_uf2_check_stuck`) resets it; if the partial app can't boot, the app-handshake watchdog sends it back to the bootloader after 3 tries. Then use *stuck in update mode*. | Device stays in its bootloader (`AdaDFU`): use *stuck in update mode*, over Bluetooth or USB |

iPhone/iPad browsers have neither Web Serial nor Web Bluetooth and are unsupported. Firefox and Safari can't run the checks either. Anyone can still download a release `.uf2` from GitHub and drag it onto the drive, but the page doesn't offer that unchecked path.

### Known limits (vs. deploy_local.py)

- **USB drive identity:** a browser can't read the drive's USB serial, so the drive isn't pinned to the chip before writing. Mitigations:
  - the `INFO_UF2.TXT` board/bootloader check;
  - the post-flash serial-number check.

  Writing to another XIAO installs the same stable release there and fails verification; the user's own device stays in its bootloader with the old app.
- **AdaDFU ambiguity:** Web Bluetooth hides addresses, so two devices in update mode look alike in the picker. The user is told to stay next to their device, and the post-flash serial check catches a wrong pick.
- **No server-side audit log:** the page shows a copyable log instead.

## Bench gate: outstanding before announcing the page

None of the browser-to-hardware paths has run on a device yet. All the logic is unit-tested, against a simulated bootloader and against the Python implementations. Run each item on a **bare test chip first** (listed in `devices/test_devices.json`, reset button + SWD pads reachable), then on one enclosed test device:

- [ ] **USB, File System Access:** Chrome/Edge on Windows, macOS and ChromeOS let `showDirectoryPicker` open the `XIAO-SENSE` drive root. Chrome writes via a `.crswap` swap file and renames on `close()`; confirm the bootloader flashes from the swap-file blocks and that the torn `close()` is reported as expected.
- [ ] **USB, re-enumeration:** after the update, `navigator.serial.getPorts()` returns the app port without a second picker. Otherwise the page falls back to asking; note which happens.
- [ ] **USB, manual copy:** the download + drag path completes and verifies.
- [ ] **BLE, Android Chrome:** full update, with timing at 20-byte packets. If it's too slow, measure a larger `DFU_CHUNK`; the PRN byte-count check makes a truncating link fail fast and safely.
- [ ] **BLE, desktop Chrome** (Windows / macOS): same.
- [ ] **BLE, reconnect:** after DFU, the same `BluetoothDevice` reconnects to the app without a picker.
- [ ] **USB, interruption:** unplug mid-write. Confirm that the device ends up back in its bootloader (stuck-transfer timeout, or the handshake watchdog after 3 boots) and that *stuck in update mode* completes the update.
- [ ] **BLE, interruption:** close the tab mid-transfer. The device stays in `AdaDFU`, and *stuck in update mode* finishes the update over BLE and, separately, over USB.
- [ ] **Refusals on hardware:** an unconfigured device; a device up less than 5 min (the page waits); a downgrade and a settings change (the page asks).
- [ ] **Mirror:** promote a release and check that the console redeploys with it as the default; roll back and check that the default follows.

## Fleet interplay

- **A personal device joins on its own.** There is no enrollment or network ID: the firmware obeys any in-range broadcaster, and blinky-server adopts any `Blinky-*` advertiser as `PRESENT`.
- **Fleet deploys reach personal devices.** `./scripts/deploy.sh --devices=all` includes every in-range nRF52840, personal ones too. Fleet and the web flasher both install only stable releases, so the worst case is a personal device being moved to the latest stable.
- **Pinned personal devices get moved.** A personal device pinned to an older stable release on purpose is moved to the latest stable by the next fleet-wide deploy while it's in range.
