# Build service

Builds commits of [blinky-firmware](https://github.com/Jdubz/blinky-firmware) into the same dev package a staging build makes: `blinky-things-b<N>.hex`, `blinky-things-b<N>.uf2` and `manifest.json` (sha256s, `channel: dev`, `git_sha`, version `b<N>-dev-<sha8>`). The box verifies it the way it verifies a staging artifact: `blinky-server/hosttools/firmware_release.py`, `verify_dev_package` (or `firmware_release.py verify-dev-package --sha SHA --dir DIR`). The design and its trust rules are in [docs/DEV_LOOP.md](../docs/DEV_LOOP.md).

This directory is the public code and image definitions. Its deployment is `deploy/build-host/build/` (RUNBOOK there), run by the build host's pull deployer for gated master commits. Where it runs (address, port, CPU set, token reference) is private: a gitignored `build.env` on the build host, and the token in 1Password. Nothing here has a default address.

## How it works

Two containers share one job volume. Neither has a docker socket.

- **`api`** (`blinky_build/api.py`): HTTP with a bearer token, and every git operation. It never runs repository code: no hooks, no system or global git config, no submodules.
  - It keeps a bare mirror of the firmware repo that fetches only `+refs/heads/*:refs/heads/*`, never a commit by SHA.
  - **Trust:** a commit builds only if it's on one of the repo's own branches (an ancestor of a branch head in the mirror). GitHub serves a fork PR's commits by SHA (`refs/pull/*`), so "the SHA exists" proves nothing.
  - **Bundles** (unpushed commits from the box): every prerequisite must be a commit on a branch. The bundle is fetched into `refs/dev/<id>/*`, never `refs/heads/*`.
  - It writes the job (a checkout with history) into the job volume.
- **`worker`** (`blinky_build/worker.py`): `network_mode: none`, uid 10001, all capabilities dropped, `no-new-privileges`.
  - It runs the firmware's own steps exactly as `staging-build.yml` does: `scripts/build.sh --dev`, `tools/firmware_release.py package --dev`, the `Version.h` check.
  - The toolchain is baked in at image build time by the firmware's `scripts/setup_toolchain.sh` at `FIRMWARE_TOOLCHAIN_REF`. It's root-owned, so a build can't change it or swap it: the checkout directory around it is root's, group 10001, sticky (`1775`), so the build creates and removes its own files there but can't rename or replace `.arduino` (`verify.sh` and CI probe both).
  - A commit whose pin differs fails with `toolchain mismatch: rebuild the worker image`. The pin is `tools/firmware_toolchain.json` (minus comments), `TFLITE_REF` and the `patches/*.patch` hashes (`blinky_build/toolchain.py`).
  - **Incremental builds:** the build tree is one persistent checkout at a fixed path, moved between commits with `git checkout`, and arduino-cli's build directory is on the cache volume (one per toolchain). Only changed files recompile.
  - **Why the rootfs isn't read-only:** arduino-cli resolves library symlinks and the firmware's `check_build_libs.py` wants the libraries under `<checkout>/.arduino`. So the tree lives in the container layer next to the baked toolchain.

## API

| Request | Result |
|---|---|
| `GET /v1/healthz` (no auth) | `{"ok": true}` |
| `POST /v1/builds`, JSON `{"repo": "Jdubz/blinky-firmware", "sha": "<40 hex>"}` | 202 `{"id", "state": "queued", "sha"}` |
| `POST /v1/builds`, multipart `bundle` (file) + `sha` | the same |
| `GET /v1/builds/{id}` | `{"id", "state": "queued\|building\|done\|failed", "sha", "version", "error", "log_tail", ...}` |
| `GET /v1/builds/{id}/package` | `.tar.gz` of `manifest.json`, `*.hex`, `*.uf2` (the `staging-firmware-<sha>` artifact's files); 409 until done |
| `POST /v1/checks`, JSON `{"repo": "Jdubz/blinky-firmware", "sha": "<40 hex>", "check": "tracker-harness" \| "tracker-harness-nn"}` | 202 `{"id", "state": "queued", "sha", "check"}` |
| `GET /v1/checks/{id}` | `{"id", "state", "sha", "check", "corpus", "conclusion": "success\|failure" (once done), "steps": [{"name", "outcome", "seconds"}], "error", "log_tail", ...}` |

- **Checks** (`blinky_build/checks.py`; [DEV_LOOP.md](../docs/DEV_LOOP.md), section 1): one heavy job of the commit's own `pr-validation.yml`, run step for step in the worker's sandbox with the corpus the commit pins. Branch commits only (no bundles), and the corpus pin must be on a branch of blinky-corpus (its own `refs/heads/*` mirror; `BLINKY_BUILD_CORPUS_REPO`, `BLINKY_BUILD_CORPUS_REMOTE`). `state: failed` is an infrastructure error (a step the image can't run as written, a toolchain mismatch); a failing gate is `done` with `conclusion: failure`. 400 also means no corpus pin; 403, the commit or its corpus pin is not on a branch.
- **Repeat requests:** a SHA with a `done` build gets that build back (202 with `"state": "done"`), and so does a SHA with a build still queued or building. A failed build is built again.
- **Errors:**
  - 400: bad input, a malformed bundle, or a SHA in neither the bundle nor a branch;
  - 401: no token, or a bad one;
  - 403: not on a branch, with the reason;
  - 409: bundle prerequisites not on a branch;
  - 413: too large.

## Configuration

| Variable | |
|---|---|
| `BLINKY_BUILD_HOST`, `BLINKY_BUILD_PORT` | api: where to listen (required) |
| `BLINKY_BUILD_TOKEN` or `BLINKY_BUILD_TOKEN_FILE` | api: bearer token, at least 32 characters (required) |
| `BLINKY_BUILD_REPO`, `BLINKY_BUILD_REMOTE` | api: `Jdubz/blinky-firmware`, its GitHub URL |
| `BLINKY_BUILD_MAX_BUNDLE_MB`, `BLINKY_BUILD_KEEP_JOBS` | api: 64, 200 |
| `BLINKY_BUILD_TIMEOUT` | worker: seconds per step, 1800 |
| `FIRMWARE_TOOLCHAIN_REF` | worker image build arg: a blinky-firmware commit on a branch |

## Running it

- **Tests:** `python -m pytest tests` (stdlib and git only).
- **End to end:** CI runs it on GitHub-hosted runners (`.github/workflows/pr-validation.yml`, job "Build service"). It builds both images, starts `compose.yaml`, builds a staging commit by SHA and a new commit by bundle, and verifies both packages.
- **Locally:** `BLINKY_BUILD_BIND=127.0.0.1 BLINKY_BUILD_PORT=... BLINKY_BUILD_TOKEN=... FIRMWARE_TOOLCHAIN_REF=<sha> docker compose up -d --build`.
- **Not on the test box:** it builds no images and runs no workloads.
