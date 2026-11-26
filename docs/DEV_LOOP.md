# Dev loop: build, flash and test on the box before CI

Status: design, agreed with the owner on 2026-11-25. Phases are at the end. Built: phase 1 (the build service, [build-service/](../build-service/README.md); box modes, dev deploy and targeted checks, `dev.py`), phase 2 (the PR hardware queue: `scripts/testbox/pr_queue.py`; [TEST_BOX.md](TEST_BOX.md), "PR hardware queue") and phase 2.1 (the blocking hardware checks, the server's `test.hardware-checks` job, in place of phase 2's smoke test; [TEST_BOX.md](TEST_BOX.md), "Hardware checks"). The owner makes `Hardware test (PR)` a required status on blinky-firmware's `staging` ruleset.

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

1. **Builds run on the build host, through a build service.** That keeps the box light and builds faster. The repos are public: they name no host, address, port or secret. Where the service runs comes only from private config (see "Where it runs").
2. **Draft PRs run no heavy CI jobs** until they're marked ready for review.
3. **On-box tests have two tiers** (refined the same day). First, simple yes/no hardware checks that block: upload (does the firmware run), lights (do the LEDs work), audio (does the mic work), response (does each analysis feature respond to audio), then serial, BLE and the rest. They are not quality checks. Then the full suite, which scores the algorithm and never blocks. A PR staging → master must pass the hardware checks, and a red commit must not reach the history.
4. **The top-scoring builds are proposed as stable candidates.** The owner decides; promotion stays `release_promote.py`.
5. **Dev mode has no expiry.** Work waiting behind it must be obvious, and leaving dev mode is a conscious switch. Automating that comes later, once real use shows what the signals are.
6. **No untrusted code runs on the owner's machines.** Anyone can open a PR on a public repo. See "Trust".

## Trust

This is the rule everything else follows.

- **No GitHub runner on the build host ever takes a job from a public repo.** blinky's workflows run only on GitHub-hosted runners (checked 2026-11-25: all 14). The org's self-hosted runners serve private org repos only; their runner group has public repos off, and the blinky repos are on the personal account. Keep it that way: "CI jobs on the build host" means jobs submitted to the build service, never a runner registration.
- **The build service builds only owner commits.** A request names a commit of `Jdubz/blinky-firmware`. The service refuses it unless the commit is on one of that repo's own branches, which only the owner can push. "The SHA exists in the repo" is not enough: GitHub serves a fork PR's commits from the base repo by SHA (`refs/pull/N/head`). The check is local: the service's mirror fetches only the branch heads (`refs/heads/*`), never a commit by SHA, and the commit must be an ancestor of one of them.
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

- **API** (bearer token; the contract is in [build-service/README.md](../build-service/README.md)):
  - `POST /v1/builds` takes JSON `{"repo": "Jdubz/blinky-firmware", "sha"}` or multipart `bundle` + `sha`, and answers 202 `{"id", "state", "sha"}` (`state: done` at once on a cache hit; a commit already building returns that build); 403 is an untrusted commit, 409 a bundle whose prerequisite commits the service lacks;
  - `GET /v1/builds/{id}` returns `{"id", "state": "queued|building|done|failed", "sha", "version", "error", "log_tail", "source", "created_at", "duration_s"}`;
  - `GET /v1/builds/{id}/package` returns a `.tar.gz` of `manifest.json`, `*.hex` and `*.uf2`: the files of a `staging-firmware-<sha>` artifact.
- **Output:** the same dev package as a staging build (`firmware_release.py package --dev`): hex, UF2, and `manifest.json` with sha256s and version `b<N>-dev-<sha8>`. The box verifies it exactly as `download_staging` verifies a staging artifact: both call `verify_dev_package` in `blinky-server/hosttools/firmware_release.py` (every file listed with its sha256, nothing unlisted).
- **Speed:** a warm Arduino build cache per toolchain version (`tools/firmware_toolchain.json`) and a persistent build tree, so an incremental build recompiles only what changed. One build at a time for now; arduino-cli already compiles on every core in the CPU cap. The target is well under a minute for an incremental build.
- **Later (phase 3):** it also runs the heavy CI jobs, the tracker harness and the NN harness, for owner commits, posting the results as commit statuses with a token scoped to commit statuses on these repos.

### 2. Box modes: `dev` and `queue`

- **Where the mode lives:** in blinky-server, persisted (`GET`/`PUT /api/box/mode`). The default is `queue`.
- **`queue` mode:** the hardware runners (staging and the PR queue) submit their work, whichever ready item has waited longest first.
- **`dev` mode:** the runners submit nothing. Each waiting commit gets a pending status saying why: `queued: box in dev mode since 14:05 (3 waiting)`. The same shows in `dev.py status`, in metrics on the box's Grafana dashboard (`blinky_box_mode`, `blinky_hw_queue_depth`, `blinky_hw_queue_oldest_seconds`), and in the staging reports.
- **No expiry:** the box stays in dev mode until someone switches it back.
- **Switching into dev mode while a suite runs** waits for the suite to finish, or `--preempt` cancels it and puts it back in the queue.
- **How it is built** (`blinky_server/testing/box_mode.py`, `scripts/testbox/staging.py`):
  - the mode is a JSON file in the server's data dir, so it survives restarts;
  - while a switch waits for its suite the box is "switching": still `queue`, but the runners hold back and `POST /api/test/staging` answers 409 (closing the race of a runner that read the mode just before the switch);
  - each runner reports what it holds back (`PUT /api/box/queue/{runner}`), which is what `GET /api/box/mode`, `dev.py status` and the metrics show. In queue mode it reports what waits for the box too, ready items marked `ready`; the server stamps each runner's last report (`reported_at`), and a runner yields to another's older ready item only while that runner still reports;
  - a preempted hardware test (hardware checks or the staging quality suite) is cancelled at its next step boundary (a flash in flight finishes) and listed in the mode's `preempted`; its runner sees its job there, records no failure, and tests that head again.

### 3. Dev flash and targeted tests (dev mode only)

- **`POST /api/dev/deploy {build_id, device_ids}`:** the staging deploy pipeline unchanged, with a build-service package in place of the staging artifact. Test chips only, a verified package, the post-deploy version assertion, and an image stored as non-stable, so auto-recovery stays on the stable image (#251). The server fetches the package itself and checks it as `download_staging` checks a staging artifact (`verify_dev_package`, the same function: manifest `channel: dev`, `git_sha`, `version == b<N>-dev-<sha8>`, the sha256 of every file, nothing unlisted), and unpacks only flat regular files, size- and count-capped. It is a durable job (`fleet.deploy-dev`) holding the audio lock, and the box's relaxed path like `deploy-staging`: the API key and box chips only, never the `X-Deploy-Tool` header.
- **`POST /api/dev/run {checks}`:** any of the following, run in order inside one test session:
  - `info`: the `json info` timers, frame time and heap;
  - `health --minutes N`;
  - `validate --tracks …`;
  - `pace`;
  - `camera --tracks …` (`dev.py try --camera`: the existing `camera_review.py`, which submits its own validation job);
  - `commands …` (no reboot or config write: those aren't checks).

  These are the staging suite's own functions; the suite becomes the "everything" set. A durable job (`test.dev-run`): the audio lock and the LEDs lit for the whole run, runtime settings restored once at the start (so a `commands` check's `set` holds for the checks after it), streaming only while a track plays. Health and the end-of-run asserts (no reboot, no `[ASSERT]`/`[FALLBACK]`) set pass/fail; scores are reported, never gating.
- **`scripts/testbox/dev.py`** (stdlib only, run from a blinky-firmware checkout or with `--repo`):
  - `mode dev|queue [--note] [--preempt]`;
  - `status`: the mode, what waits behind it, the chips;
  - `build [REF] [--wip]`;
  - `flash BUILD_ID`;
  - `test <checks>`: `info`, `health[=MIN]`, `validate=a,b`, `pace`, `cmd=c1;c2`; `test --hw`: the hardware checks on the chip as it is (`POST /api/dev/hardware-checks`);
  - `try [REF] [--hw] [--tracks a,b] [--health MIN] [--pace] [--camera] [--wip]`: the one command (`--hw`: the hardware checks after the flash). It enters dev mode itself if needed and says so, builds, flashes, runs the info check plus the ones asked for, and prints a short report (version, flash result, each check, the info timers). The box stays in dev mode afterwards.
- **No push needed.** `dev.py` sends an unpushed commit as a git bundle of `<base>..<sha>`, where base is the merge-base with a fetched `origin/staging` (or `origin/master`). If the service answers 409 (it lacks the base), it tries the merge-base with the other branch, then says what to push. A commit already on a branch goes by sha.
- **`--wip`.** A dirty tree is refused, unless `--wip`: the working tree, untracked files that git doesn't ignore included, becomes a throwaway commit on HEAD, through a temporary index and `git commit-tree -p HEAD`, kept at `refs/dev/wip`. The branch, index and stash are never touched, and the build is still of a commit (see "Trust").
- **The 60 s rule.** A dev deploy is refused, with Retry-After, until each target chip has run 60 s since its last reset; `dev.py` waits that out once.
- **CLAUDE.md** also loses "The build host runs one-shot CPU scripts only, with nothing left running": the build service is the exception the owner chose.
- **CLAUDE.md** changes from "dev builds only through `deploy_local.py`" to "dev builds through `deploy_local.py`, or the box's dev mode".

### 4. CI: drafts run no heavy jobs

- **blinky-firmware's PR Validation** runs only the cheap checks on a draft: changed paths, syntax, code quality, docs, static init and LED mapper. Compile, cppcheck, the tracker harness, the NN harness and the timing job wait for `ready_for_review`, which is added to the triggers.
- **blinky_time** gets the same treatment for the server, console and ML suites.
- **The Claude review** already skips drafts.
- **During dev work,** builds come from the build service, not CI.

### 5. PR hardware queue (built)

- **What it takes:** ready PRs to `staging` whose head is a branch of the repo and that change firmware paths (`tools/firmware_paths.json`, against the merge-base with `staging`). Drafts are ignored. A fork PR is never built or tested on the box; it gets `success` saying so ("fork PR: not hardware-tested before merge; the staging suite tests it after merge"), so a required status can't block it forever. A PR with no firmware change gets `success`, "no firmware change: hardware test skipped".
- **Order:** the newest head of each PR, oldest PR first. A superseded head drops out (a running test is cancelled) and is marked superseded; its successor keeps its place.
- **Build:** on the build service by sha, queue mode only. A failed build posts `failure` with the log's tail in the PR comment.
- **Hardware checks** (phase 2.1: `POST /api/test/hardware-checks`, `blinky_server/testing/hardware_checks.py`, a durable job, queue mode only; [TEST_BOX.md](TEST_BOX.md), "Hardware checks"): the dev package through the `/api/dev/deploy` path, then the phases in order, stopping at the first that fails: upload, lights (the camera watching `testpattern` walk the pixels), audio, response (one yes/no per streamed analysis feature, against a synthetic stimulus), interfaces. The audio lock end to end, the LEDs lit for the run. It posts `Hardware test (PR)` on the head (`success`, `failure`, or `pending` "rig error: …, retrying": a rig fault is never red) and one report comment per tested head (`## Hardware test (PR) — <sha>`, updated in place: the checks table first, then the quality note). Scores are not in the blocking status: they are the staging quality suite's.
- **Staging confirms, it doesn't re-judge.** Squash merges of up-to-date PRs make the staging commit's tree the PR head's, so the staging runner posts `Hardware test (staging)` `success` "hardware checks passed before merge as PR #N head <sha8> (same tree)" for it without a run, and checks only a commit that matches no passing PR head. Then the quality suite (every track, beat pace, scores with the corpus pin and the scorer's lag evidence) runs on the staging head, non-blocking: its status, `Quality suite (staging)`, is always `success` with a score summary.
- **Dev mode:** nothing submitted; each queued head pending with `queued: box in dev mode since HH:MM (N waiting)`. A PR closed or merged while queued or running is dropped (its test cancelled) with nothing more posted; a preempted test goes back in the queue.
- **The box is shared** with the staging runner: one hardware test at a time (409: next pass), whichever ready item has waited longest first.
- **Staging** keeps skipping images whose firmware fingerprint is unchanged.

### 6. Stable candidates from scores

- **Recording:** each staging quality suite records its scores with the corpus pin and the test setup (built in phase 2.1: `staging-scores.jsonl` in the server's data dir, one line per commit, each track's drum scores with `lagSource`/`lagAmbiguous`). Scores are compared only within the same pin.
- **Lag:** a box run's drum scores depend on removing its lag (ffplay's start to detection: 0.24-0.53 s, varying run to run). The scorer searches for it over the rig's window and reports where it came from (`lagSource`, `lagAmbiguous`, `lagNote`: blinky-corpus `blinky_scoring/lag.py`). An ambiguous lag (periodic drums) or a fallback is flagged in the score, not hidden. Box drum scores from before corpus `a20aa2f` were scored at an aliased lag and aren't comparable with later ones.
- **Proposing:** when a candidate leads the current stable without health regressions (drum F, kick/snare recall, on-drum, repS, tempo, each beyond its noise), the box opens a "Propose bN for stable" issue with the comparison. The owner decides.
- **No composite score yet:** the proposal shows each metric. How to weigh them gets decided from real proposals.

## Phases

1. **The dev loop:**
   - the topology leaks out of the public repo (see "Where it runs");
   - the build service on the build host (owner commits and bundles, sandboxed);
   - box modes;
   - dev flash and targeted tests, and `dev.py`;
   - drafts run no heavy jobs.
2. **The PR hardware queue:** `Hardware test (PR)` on each firmware PR head (built).
   - **2.1, the blocking hardware checks:** upload, lights, audio, response, interfaces, yes/no, on the PR head; staging confirms them by tree; the quality suite non-blocking after (built). `Hardware test (PR)` becomes required on blinky-firmware's `staging` ruleset by the owner.
3. **The build service takes the heavy CI jobs for owner commits; score-ranked stable proposals.**

Leaving dev mode stays manual until phase 1 has been used enough to show what should trigger it.
