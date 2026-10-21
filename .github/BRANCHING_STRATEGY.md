# Branching Strategy and Workflow

## 🌳 Branch Structure

### `master` - The only long-lived branch
- **Purpose**: Integration branch. Every change lands here by pull request.
- **Protection**: Repository ruleset (see [BRANCH_PROTECTION.md](BRANCH_PROTECTION.md)): PR required, required CI checks, no force-push or deletion.
- **CI/CD**: `.github/workflows/pr-validation.yml` runs on every PR to master and on every push to master.
- **Releases**: Merging does **not** ship firmware. Releases are cut from master separately (see below).

### Feature Branches
- **Naming**: by kind of change, e.g. `fix/description`, `feat/description`, `docs/description`
- **Branch from / merge to**: `master`
- **Lifecycle**: one change per branch; delete after merge

> The old `develop` → `staging` → `master` flow, with date-stamped staging versions and semver production releases, is retired. The `staging` branch on GitHub is stale; don't target it.

## 🔄 Workflow Process

### 1. Make the change
```bash
git checkout master
git pull origin master
git checkout -b fix/my-change

# Work, compile with ./scripts/build.sh, test on a test device
git add <files>
git commit -m "fix: describe the change"
git push origin fix/my-change
```

### 2. Pull request to master
- Open a PR against `master`.
- The required CI checks must pass. An automated Claude code review also runs (`.github/workflows/claude-code-review.yml`).
- Merge when green.

### 3. Release (separate from merging)
Firmware releases follow [docs/RELEASE_PROCESS.md](../docs/RELEASE_PROCESS.md):
1. `gh workflow run release.yml --ref master` builds a **candidate** (pre-release `bN`) from a green master commit.
2. `python tools/release_gate.py bN ...` bench-gates it on a test device.
3. `python tools/release_promote.py bN` makes it **stable** (tag `stable-bN`, marked *Latest*, signed for OTA).

Only stable releases reach fleet and personal devices.

## 🤖 Versioning

Firmware is versioned by build number, not semver:
- **Release:** `bN`. The release workflow sets `N = max(BUILD_NUMBER, highest bN tag) + 1`.
- **Dev build:** `bN-dev-<sha>` (`./scripts/build.sh --dev`), for a test device only.
- **Settings format:** `SETTINGS_VERSION` in `blinky-things/config/ConfigStorage.h`, independent of the build number. A bump resets saved tuning on every device; release notes flag it.

## 🧪 Testing Strategy

### On every PR to master and every push to master
`pr-validation.yml` runs, among others:
- Compile Firmware (pinned toolchain, `scripts/build.sh`)
- Static Analysis (cppcheck), Static Init Safety Check, Syntax Validation, Code Quality, Documentation Check
- OTA Manifest (firmware verifier vs release signer)
- Absolute Timing (generators + rhythm phase)
- Tracker Harness (UBSan stress + corpus gate), Tracker Harness with real onset NN, Onset Model Input Check
- Feature Parity (Python ↔ firmware)
- Blinky Server Tests, Blinky Console Tests

See [tests/README.md](../tests/README.md) for the host tests.

### On a release
The release workflow refuses a commit that is not on master or has no successful PR Validation run. The candidate then needs the bench gate before promotion.

## 🚨 Emergency Fixes

There is no separate hotfix path: branch from master, open a PR, merge when CI is green, then cut and gate a release as usual. To get a device off a bad build quickly, re-mark an earlier stable release as Latest (`python tools/release_promote.py bM --rollback`) and deploy it; see [docs/RELEASE_PROCESS.md](../docs/RELEASE_PROCESS.md).
