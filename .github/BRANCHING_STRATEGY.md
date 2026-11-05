# Branching Strategy and Workflow

This is the tooling repo (#357). The firmware has its own flow in [blinky-firmware](https://github.com/Jdubz/blinky-firmware): feature PRs go to `staging`, the test box hardware-tests the staging head, and a pass merges it to `master`, which cuts a release candidate. See that repo's `docs/RELEASE_PROCESS.md`.

## Branches

### `master`: the only long-lived branch
- **Purpose**: the integration branch. Every change lands here by pull request.
- **Protection**: a repository ruleset (see [BRANCH_PROTECTION.md](BRANCH_PROTECTION.md)): PR required, required CI checks, Copilot review, no force-push or deletion.
- **CI**: `.github/workflows/pr-validation.yml` on every PR to master and every push to it; `.github/workflows/claude-code-review.yml` on every PR.
- **Deploys**: merging is what changes the test box's blinky-server. Its deployer moves to a merged commit that touches the server's paths once CI is green and the PR's last commit has a clean Claude review verdict (CLAUDE.md). `blinky-console-deploy.yml` deploys the console from master.

### Feature branches
- **Naming**: by kind of change: `fix/...`, `feat/...`, `docs/...`, `ci/...`.
- **Branch from / merge to**: `master`. One change per branch; delete it after merge.

### Dependabot branches
Dependabot opens daily PRs that bump the `firmware/` (blinky-firmware `master`) and `corpus/` (blinky-corpus `main`) submodule pins (`.github/dependabot.yml`). They go through the same CI and review as any PR, so the parity check and the server tests run against the new pin before it is merged.

## Workflow

```bash
git checkout master && git pull
git checkout -b fix/my-change
# work, run the tests for the part you changed (CONTRIBUTING.md)
git commit -m "fix: describe the change"
git push -u origin fix/my-change
gh pr create --base master
```

Merge when the required checks are green and the review findings are addressed.

## Emergency fixes

There is no separate hotfix path: branch from master, open a PR, merge when CI is green. To get devices off a bad firmware quickly, see blinky-firmware's `docs/RELEASE_PROCESS.md` (rollback re-marks an earlier stable release as Latest), then deploy it with `scripts/deploy.sh`.
