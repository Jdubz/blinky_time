# Firmware Release Process

**Rule:** only firmware that passed CI *and* a bench gate reaches fleet or new devices. Broken and in-flight changes stay on test chips.

**Why:** until 2026-10-20, `deploy.sh` and `tools/deploy_local.py` compiled whatever was in the local working tree, uncommitted changes included, and flashed it. b210 (NN drain-loop livelock, 12 fps) and b211 (mic silenced by a noise gate) both reached hat_v1 that way.

## Channels

| Channel | What it is | Where it may go |
|---|---|---|
| **stable** | GitHub Release `bN` marked *Latest* + tag `stable-bN` | Default for every deploy and for new devices |
| **candidate** | GitHub *pre-release* `bN`, built by CI from a green master commit | One listed test device at a time, until gated |
| **dev** | Local build of a clean tree, version `bN-dev-<sha>` | `--dev`, one listed test device |

Test devices are listed in `devices/test_devices.json`. Being a test device is a deliberate property of the device, not where it happens to be plugged in. Candidates and dev builds are refused for every other device.

## Cutting a release

A release is a vetted checkpoint that the owner picks. **Nothing releases on its own:** merging to master builds nothing for devices. Each step below is run by hand. b215 is the current stable and the first signed release.

### 1. Candidate (CI)
```bash
gh workflow run release.yml --ref master     # or: Actions → "Release candidate" → Run
```
`.github/workflows/release.yml`:
1. **Gates:** refuses unless the commit is on master with a successful `PR Validation` run.
2. **Build number:** `N = max(BUILD_NUMBER, highest bN tag) + 1`.
3. **Pinned toolchain:** `tools/firmware_toolchain.json`, TFLite pinned in `tools/setup_tflite_lib.sh`, and the core patches from `scripts/apply_core_patches.sh`.
4. **Build:** `scripts/build.sh --build N`.
5. **Package:** `tools/firmware_release.py package`. It runs the same hex/UF2 validators as the deploy paths, strictly (a skipped safety check is an error), and writes `manifest.json`: version, git sha, sha256 per file, toolchain, `SETTINGS_VERSION` / `DEVICE_VERSION`. A candidate is **not** signed for OTA; that happens at promotion, after the gate.
6. **Publish:** pre-release `bN` with `blinky-things-bN.hex`, `.uf2` and `manifest.json`. The notes list the changes since the previous release and **flag a `SETTINGS_VERSION` change**, because that resets saved tuning on every device.

The PR Validation `Compile Firmware` job installs the same pinned toolchain, so every PR compiles what a release would.

## The pinned toolchain (every machine)

Every build (CI, the release workflow, a workstation, blinkyhost) uses the same isolated toolchain. **arduino-cli itself is pinned too** (`arduino_cli` in `tools/firmware_toolchain.json`); a different version is refused (CI's setup-arduino-cli used to install the newest release, even a release candidate). Note: Windows and Linux builds of the same commit with identical toolchain versions and libraries differ by ~48 bytes of flash (RAM identical), most likely path-dependent strings. Shipped firmware is unaffected because releases are only ever built by CI. `bash scripts/setup_toolchain.sh` installs the rest into `<repo>/.arduino` (gitignored):
- the core pinned in `tools/firmware_toolchain.json`;
- exactly the libraries pinned there, plus TFLite at the commit in `tools/setup_tflite_lib.sh`;
- the core patches from `scripts/apply_core_patches.sh`.

`scripts/arduino_env.sh` points arduino-cli at that directory through `ARDUINO_DIRECTORIES_*`. `build.sh` and the Makefile (`scripts/in_toolchain.sh`) build only inside it.

Three layers make the wrong USB library impossible to ship (#252):
1. **Isolation:** a machine's own Arduino folders are never read, so nothing can shadow a pinned library.
2. **Library check:** `tools/check_build_libs.py` parses the verbose compile log and fails the build if:
   - a library resolves outside `.arduino/`;
   - arduino-cli had to choose between multiple copies of a library;
   - a pinned version drifts;
   - TinyUSB is not the core's.

   The resolved set is recorded in each release's `manifest.json`.
3. **Compile-time guard:** the patch set defines `BLINKY_TINYUSB_CDC_PATCHED` in TinyUSB's `cdc_device.h`, and `blinky-things.ino` `#error`s without it. Any build that links another TinyUSB fails, whatever the build path.

Changing the toolchain means editing the pin files, re-running `setup_toolchain.sh`, and checking that CI and a local build produce the same sizes.

### 2. Bench gate (test device)
```bash
python tools/release_gate.py bN --port COM3 --device-id hat_v1
```
Flashes the candidate through `deploy_local.py --release bN`, then checks:

| Check | Pass |
|---|---|
| version | `json info` reports exactly the manifest version |
| configured | `device.configured` true, same `device.id` |
| fps | ≥ 60 for the whole window (`--min-fps`). A disaster floor (b210 shipped at 12 fps), not a quality bar: hat_v1 runs about 100–175 fps with music on both b214 and b215. What matters is timing: see frametime below (#258). |
| frametime | every 5 s window's longest frame (`json info` `maxFrameMs`) ≤ 100 ms (`--max-frame-ms`). hat_v1 with music peaks at 24–31 ms; a long frame is a visible freeze (#258, #275). |
| stability | no reboot for 5 min (`--stable-minutes`); `audioOverruns` grows ≤ 5 |
| asserts | no `[ASSERT]` / `[FALLBACK]` on the console |
| music | with music playing (it prompts): level mean ≥ 0.05, ≥ 1 pulse onset/s |
| operator | a human confirms the visuals look right (prompt, or `--operator-confirmed "<note>"`) |

The result is attached to the release as `gate-<device>.json`, pass or fail.

### 3. Promote
```bash
python tools/release_promote.py bN
```
Refuses without a passing gate record for this exact version. It then dispatches `sign-release.yml`, which signs the release for signed OTA (the raw image `blinky-things-bN.bin` and its Ed25519-signed manifest `.otam`) and re-checks the gate. The workflow runs only from master. The key, `BLINKY_RELEASE_SIGNING_KEY`, is a secret of the GitHub environment `release-signing` (master only) and is used nowhere else. It waits for the run, and verifies the signature against the key in `blinky-things/ota/OtaKeys.h`. Only then does it tag `stable-bN` and mark the release stable and *Latest*.

Promotion then dispatches a blinky-console redeploy (`gh workflow run blinky-console-deploy.yml`). That deploy mirrors the newest stable releases for the web flasher (`#firmware`, [WEB_FLASHER.md](WEB_FLASHER.md)), so personal devices are offered `bN` by default.

### Rollback
```bash
python tools/release_promote.py bM --rollback    # bM: an earlier stable release
```
Re-marks `bM` as *Latest*. Nothing is rebuilt; the next deploy installs `bM`. It also dispatches a blinky-console redeploy, so the web flasher's default follows. **After deleting a release by hand**, run `gh workflow run blinky-console-deploy.yml --ref master` yourself: tag and release events run the *tagged* commit's workflow file, which for an older release may not have the trigger.

## Deploying

| Command | Installs |
|---|---|
| `./scripts/deploy.sh --devices=all` | latest stable, downloaded and sha256-verified |
| `./scripts/deploy.sh --devices=ID --release bN` | a specific **stable** release (pin or roll back a subset) |
| `python tools/deploy_local.py --port COMx --device-id ID` | latest stable, for a device on this machine's USB |
| `python tools/deploy_local.py ... --release bN` | release `bN`; a candidate only if `ID` is a test device |
| `python tools/deploy_local.py ... --dev` | local build of a clean tree (test device only) |

**`deploy.sh` (via blinky-server) is stable-only.** The server keeps the last uploaded image for auto-recovery and could re-flash it to production devices (#251). So candidates and dev builds go only through `deploy_local.py`, to a test device on this machine's USB, until the server binds recovery to an immutable, stable-only image.

A clean tree means no tracked change anywhere in the repo (except the generated `Version.h` / `BUILD_NUMBER`) and no untracked file under `blinky-things/`. That's the same rule `gen_version.sh` uses for `-dirty`, and it's checked *before* building. `--already-in-bootloader` resumes only accept stable firmware, because they can't read the board's device id.

Releases are never compiled at deploy time. Both tools verify that the device reports **exactly** the expected version. `deploy_local.py` records the channel and release in its audit log.

## Day-to-day development
- **Iterate:** use `--dev` on one test device. It needs a clean tree: commit first, then flash. The version string carries the commit, so the build on the device is always traceable.
- **Ship:** when the owner decides master is worth releasing, cut a candidate, gate it on the test device, promote. A merge alone ships nothing.
- **Local `build.sh`:** a plain build (with bump) still works for compile checks. Nothing flashes it except `--dev`, and that refuses a dirty tree.
