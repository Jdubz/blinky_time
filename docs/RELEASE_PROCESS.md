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

### 1. Candidate (CI)
```bash
gh workflow run release.yml --ref master     # or: Actions → "Release candidate" → Run
```
`.github/workflows/release.yml`:
1. **Gates:** refuses unless the commit is on master with a successful `PR Validation` run.
2. **Build number:** `N = max(BUILD_NUMBER, highest bN tag) + 1`.
3. **Pinned toolchain:** `tools/firmware_toolchain.json`, TFLite pinned in `tools/setup_tflite_lib.sh`, and the core patches from `scripts/apply_core_patches.sh`.
4. **Build:** `scripts/build.sh --build N`.
5. **Package:** `tools/firmware_release.py package`. It runs the same hex/UF2 validators as the deploy paths, strictly (a skipped safety check is an error), and writes `manifest.json`: version, git sha, sha256 per file, toolchain, `SETTINGS_VERSION` / `DEVICE_VERSION`.
6. **Publish:** pre-release `bN` with `blinky-things-bN.hex`, `.uf2` and `manifest.json`. The notes list the changes since the previous release and **flag a `SETTINGS_VERSION` change**, because that resets saved tuning on every device.

The PR Validation `Compile Firmware` job installs the same pinned toolchain, so every PR compiles what a release would.

### 2. Bench gate (test device)
```bash
python tools/release_gate.py bN --port COM3 --device-id hat_v1
```
Flashes the candidate through `deploy_local.py --release bN`, then checks:

| Check | Pass |
|---|---|
| version | `json info` reports exactly the manifest version |
| configured | `device.configured` true, same `device.id` |
| fps | ≥ 150 for the whole window (`--min-fps`) |
| stability | no reboot for 5 min (`--stable-minutes`); `audioOverruns` grows ≤ 5 |
| asserts | no `[ASSERT]` / `[FALLBACK]` on the console |
| music | with music playing (it prompts): level mean ≥ 0.05, ≥ 1 pulse onset/s |
| operator | a human confirms the visuals look right (prompt, or `--operator-confirmed "<note>"`) |

The result is attached to the release as `gate-<device>.json`, pass or fail.

### 3. Promote
```bash
python tools/release_promote.py bN
```
Refuses without a passing gate record for this exact version. On success it marks the release stable and *Latest* and tags `stable-bN`.

### Rollback
```bash
python tools/release_promote.py bM --rollback    # bM: an earlier stable release
```
Re-marks `bM` as *Latest*. Nothing is rebuilt; the next deploy installs `bM`.

## Deploying

| Command | Installs |
|---|---|
| `./scripts/deploy.sh --devices=all` | latest stable, downloaded and sha256-verified |
| `./scripts/deploy.sh --devices=ID --release bN` | release `bN` (a candidate only if `ID` is a test device) |
| `./scripts/deploy.sh --devices=hat_v1 --dev` | local build of a clean tree (test device only) |
| `python tools/deploy_local.py --port COMx --device-id ID [--release bN \| --dev]` | same rules, for a device on this machine's USB |

Releases are never compiled at deploy time. Both tools verify that the device reports **exactly** the expected version. `deploy_local.py` records the channel and release in its audit log.

## Day-to-day development
- **Iterate:** use `--dev` on one test device. It needs a clean tree: commit first, then flash. The version string carries the commit, so the build on the device is always traceable.
- **Ship:** merge to master, cut a candidate, gate it on the test device, promote.
- **Local `build.sh`:** a plain build (with bump) still works for compile checks. Nothing flashes it except `--dev`, and that refuses a dirty tree.
