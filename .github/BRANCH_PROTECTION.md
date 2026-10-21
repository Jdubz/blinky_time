# Branch Protection Configuration

This document records the branch protection on the blinky_time repository. `master` is the only protected branch (see [BRANCHING_STRATEGY.md](BRANCHING_STRATEGY.md)).

Protection uses a **repository ruleset** named `master` (Settings → Rules → Rulesets), not classic branch protection. The ruleset targets the default branch (`master`).

## Master Branch Ruleset (current, as of 2026-10-21)

- ✅ **Restrict deletions**
- ✅ **Block force pushes** (`non_fast_forward`)
- ✅ **Require a pull request before merging**
  - Required approvals: 0
  - Allowed merge methods: merge, squash, rebase
- ✅ **Require status checks to pass** (branch must be up to date with master)
  - `Blinky Console Tests`
  - `Documentation Check`
  - `Static Analysis (cppcheck)`
  - `Static Init Safety Check`
  - `Syntax Validation`
  - `Code Quality`
- ✅ **Copilot code review** rule enabled
- **Bypass:** repository admins, for pull requests only

The required check names are the `name:` fields of jobs in `.github/workflows/pr-validation.yml`. If you rename a job, update the ruleset, or PRs will wait forever on a check that never reports.

## Checks that run but are not required

`pr-validation.yml` also runs these jobs. They are not in the ruleset, so a red result doesn't block the merge button; treat them as blocking anyway:

- `Compile Firmware` (pinned toolchain, `scripts/build.sh`)
- `OTA Manifest (firmware verifier vs release signer)`
- `Absolute Timing (generators + rhythm phase, #259-#261)`
- `Tracker Harness (UBSan stress + corpus gate)`
- `Tracker Harness with real onset NN (host TFLite Micro)`
- `Onset Model Input Check`
- `Feature Parity (Python ↔ firmware)`
- `Blinky Server Tests`

`release.yml` adds its own gate: it refuses to build a candidate from a commit without a successful `PR Validation` run on master.

## Repository Settings

- **Default branch**: `master`
- **Merge button**: merge commits, squash merging and rebase merging allowed
- **Automatically delete head branches**: off

## Checking or Changing the Ruleset

```bash
# List rulesets, then show the master ruleset's rules
gh api repos/Jdubz/blinky_time/rulesets
gh api repos/Jdubz/blinky_time/rulesets/<id> --jq '.rules'
```

Change it in the GitHub UI (Settings → Rules → Rulesets → `master`), then update this file.
