# Branch Protection Configuration

The protection on blinky_time's `master` (see [BRANCHING_STRATEGY.md](BRANCHING_STRATEGY.md)). It's a **repository ruleset**, not classic branch protection: Settings → Rules → Rulesets → "master (default branch: tooling, CI + PR)", targeting the default branch.

## Master ruleset (as of 2026-11-05)

- ✅ **Restrict deletions**
- ✅ **Block force pushes** (`non_fast_forward`)
- ✅ **Require a pull request before merging**: 0 approvals; merge, squash or rebase
- ✅ **Copilot code review** on every PR
- ✅ **Require status checks to pass** (branch up to date with master):
  - `Blinky Server Tests`
  - `Blinky Console Tests`
  - `Documentation Check`
  - `Feature Parity (Python ↔ firmware)`
  - `Trainer meets the Spark gpu_task contract (#304, #305)`
  - `ADTOF drum labeler (#284)`

The Claude review (`claude-review`) is not a required check: it's advisory and never fails a PR. Its sha-stamped verdict is what the test box deployer reads (CLAUDE.md).

A required check's name must match the job's `name:` exactly. Quote a job name that contains ` #`: unquoted, YAML reads the rest as a comment and the check gets a truncated name.

## Changing it

```bash
gh api repos/Jdubz/blinky_time/rulesets                 # list
gh api repos/Jdubz/blinky_time/rulesets/8415633 > rs.json
# edit rules / required_status_checks, then:
gh api -X PUT repos/Jdubz/blinky_time/rulesets/8415633 --input rs.json
```

Rename a check in the workflow and in the ruleset in the same step, or PRs wait for a check that never reports.

## Other repos

- **blinky-firmware**: a `staging` ruleset (the default branch: PR, Copilot review, the firmware CI checks) and a `master` ruleset (merge commits only; the same checks plus `Master takes staging only` and `Hardware test (staging)`). See its `docs/RELEASE_PROCESS.md`.
- **blinky-corpus**: a `main` ruleset (PR, Copilot review, `Scoring tests`).
