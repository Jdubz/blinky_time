# Dev loop: build, flash and test on the box before CI

Status: design, agreed with the owner on 2026-11-25. Not built yet. Phases are at the end.

## Why

A firmware change takes about an hour from push to a hardware result, and the hardware only sees it after it's merged into `staging`:

| Step | Time |
|---|---|
| PR Validation | 7–8 min (compile ~2 min; the two tracker-harness jobs, 6–8 min, set the pace) |
| Review, merge to `staging` | varies |
| `staging` re-runs PR Validation, then `staging-build.yml` | ~13 min |
| The box's staging suite | ~20 min (flash ~1.5 min, 5 min health, all 18 edm/ tracks, beat pace) |

First attempts often have mistakes, and every one of them pays the whole loop. The goal is a loop of a few minutes: build a commit, flash it to the box's test chip, run only the checks the change needs, and send it to CI only once it works.

## Owner decisions (2026-11-25)

1. **Builds run on devtop, through a build service.** That keeps the box light and builds faster. The repos are public: they name no host, address, port or secret. Where the service runs comes only from private config (see "Where it runs").
2. **Draft PRs run no heavy CI jobs** until they're marked ready for review.
3. **On-box smoke tests must finish healthy** (blocking). Their scores are reported and never block.
4. **The top-scoring builds are proposed as stable candidates.** The owner decides; promotion stays `release_promote.py`.
5. **Dev mode has no expiry.** Work waiting behind it must be obvious, and leaving dev mode is a conscious switch. Automating that comes later, once real use shows what the signals are.
6. **No untrusted code runs on the owner's machines.** Anyone can open a PR on a public repo. See "Trust".

## Trust

This is the rule everything else follows.

- **No GitHub runner on devtop ever takes a job from a public repo.** blinky's workflows run only on GitHub-hosted runners (checked 2026-11-25: all 14). The org's self-hosted runners serve private org repos only; their runner group has public repos off, and the blinky repos are on the personal account. Keep it that way: "CI jobs on devtop" means jobs submitted to the build service, never a runner registration.
- **The build service builds only owner commits.** A request names a commit of `Jdubz/blinky-firmware`. The service refuses it unless the commit is on one of that repo's own branches, which only the owner can push. "The SHA exists in the repo" is not enough: GitHub serves a fork PR's commits from the base repo by SHA (`refs/pull/N/head`). The check is a branch comparison through the GitHub API, the same one `download_staging` uses for `staging`.
- **Unpushed commits** come as a git bundle from the box. The box is an authenticated, trusted caller. The bundle must be a commit (clean tree), never a working directory: b210 and b211 reached a device from dirty trees (blinky-firmware `docs/RELEASE_PROCESS.md`).
- **Fork PRs never reach the box.** The PR hardware queue takes only PRs whose head is a branch of the repo itself. A fork PR keeps GitHub-hosted CI and gets hardware-tested only after an owner merge into `staging`, as today.
- **The build sandbox, defence in depth.** The build runs the repo's own scripts (`scripts/build.sh`), so it is arbitrary code even for owner commits. It runs:
  - as a non-root user, in a container with the pinned toolchain baked in;
  - with no network during the build (the source is fetched first) and no secrets;
  - with no docker socket and no host mounts beyond its own work and cache volumes;
  - capped on CPU, memory and PIDs, and kept off prod's reserved cores (as the runners are: cpuset 0–15).
- **The service** listens on the tailnet only and requires a bearer token. The token is stored in 1Password, rendered by the stack's deploy, and kept on the box in `~/.config/blinky/`.

## Where it runs

- **Public, in this repo:** the build service's code and container image definition, the box's client, and the server's dev endpoints.
- **Private:**
  - the deployment (host, port, CPU set, token reference): a stack in the platform's private infrastructure repo;
  - the box's client config: `~/.config/blinky/build.env` (`BLINKY_BUILD_URL`, `BLINKY_BUILD_TOKEN`), like `spark.env`.

Code reads the location from config and never has a default address.

**Existing leaks, fixed in phase 1.** `scripts/testbox/setup.sh` hard-codes the LGTM stack's tailnet address (Loki, Prometheus, Tempo), and `docs/ML_STORAGE_LAYOUT.md` names a NAS path. They move to the box's private config (`~/.config/blinky/`) and out of the docs. Tailnet addresses can't be reached from the internet, but they are topology. They stay in git history; rewriting public history isn't worth it for unreachable addresses.

## Design

### 1. Build service

- **API** (bearer token):
  - `POST /builds` takes `{repo, sha}` or a git bundle, and returns a build id;
  - `GET /builds/{id}` returns the state and log tail;
  - `GET /builds/{id}/package` returns the result.
- **Output:** the same dev package as a staging build (`firmware_release.py package --dev`): hex, UF2, and `manifest.json` with sha256s and version `b<N>-dev-<sha8>`. The box verifies it exactly as `download_staging` verifies a staging artifact.
- **Speed:** a warm Arduino build cache per toolchain version (`tools/firmware_toolchain.json`), and builds in parallel up to the CPU cap. The target is well under a minute for an incremental build.
- **Later (phase 3):** it also runs the heavy CI jobs, the tracker harness and the NN harness, for owner commits, posting the results as commit statuses with a token scoped to commit statuses on these repos.

### 2. Box modes: `dev` and `queue`

- **Where the mode lives:** in blinky-server, persisted (`GET`/`PUT /api/box/mode`). The default is `queue`.
- **`queue` mode:** the hardware runners (staging, and the PR queue from phase 2) submit their work in order, as the staging runner does today.
- **`dev` mode:** the runners submit nothing. Each waiting commit gets a pending status saying why: `queued: box in dev mode since 14:05 (3 waiting)`. The same shows in `dev.py status`, in metrics on the box's Grafana dashboard (`blinky_box_mode`, `blinky_hw_queue_depth`, `blinky_hw_queue_oldest_seconds`), and in the staging reports.
- **No expiry:** the box stays in dev mode until someone switches it back.
- **Switching into dev mode while a suite runs** waits for the suite to finish, or `--preempt` cancels it and puts it back in the queue.

### 3. Dev flash and targeted tests (dev mode only)

- **`POST /api/dev/deploy {build}`:** the staging deploy pipeline unchanged, with a build-service package in place of the staging artifact. Test chips only, a verified package, the post-deploy version assertion, and an image stored as non-stable, so auto-recovery stays on the stable image (#251).
- **`POST /api/dev/run {checks}`:** any of the following, run in order inside one test session:
  - `info`: the `json info` timers, frame time and heap;
  - `health --minutes N`;
  - `validate --tracks …`;
  - `pace`;
  - `camera --tracks …`;
  - `commands …`.

  These are the staging suite's own functions; the suite becomes the "everything" set.
- **`scripts/testbox/dev.py`:**
  - `mode dev|queue`;
  - `status`;
  - `build <ref|bundle>`;
  - `flash <build>`;
  - `test <checks>`;
  - `try <ref> <checks>`: build, flash, test, then print a short report.
- **CLAUDE.md** also loses "devtop runs one-shot CPU scripts only, with nothing left running": the build service is the exception the owner chose.
- **CLAUDE.md** changes from "dev builds only through `deploy_local.py`" to "dev builds through `deploy_local.py`, or the box's dev mode".

### 4. CI: drafts run no heavy jobs

- **blinky-firmware's PR Validation** runs only the cheap checks on a draft: changed paths, syntax, code quality, docs, static init and LED mapper. Compile, cppcheck, the tracker harness, the NN harness and the timing job wait for `ready_for_review`, which is added to the triggers.
- **blinky_time** gets the same treatment for the server, console and ML suites.
- **The Claude review** already skips drafts.
- **During dev work,** builds come from the build service, not CI.

### 5. PR hardware queue

- **What it takes:** ready PRs to `staging` whose head is a branch of the repo and that change firmware paths (`tools/firmware_paths.json`).
- **Order:** the newest head of each PR, oldest PR first. A superseded head drops out.
- **Smoke test:** flash, health, a few tracks, then no reboot and no `[ASSERT]`/`[FALLBACK]`. It posts `Hardware test (PR)` on the head. **Health must pass, and that status is required.** Scores are in the report and never block.
- **Staging** keeps the full suite with the same rule (health blocks, scores don't), and keeps skipping images whose firmware fingerprint is unchanged.

### 6. Stable candidates from scores

- **Recording:** each staging suite records its scores with the corpus pin and the test setup. Scores are compared only within the same pin.
- **Proposing:** when a candidate leads the current stable without health regressions (drum F, kick/snare recall, on-drum, repS, tempo, each beyond its noise), the box opens a "Propose bN for stable" issue with the comparison. The owner decides.
- **No composite score yet:** the proposal shows each metric. How to weigh them gets decided from real proposals.

## Phases

1. **The dev loop:**
   - the topology leaks out of the public repo (see "Where it runs");
   - the build service on devtop (owner commits and bundles, sandboxed);
   - box modes;
   - dev flash and targeted tests, and `dev.py`;
   - drafts run no heavy jobs.
2. **The PR hardware queue:** health-gated `Hardware test (PR)`.
3. **The build service takes the heavy CI jobs for owner commits; score-ranked stable proposals.**

Leaving dev mode stays manual until phase 1 has been used enough to show what should trigger it.
