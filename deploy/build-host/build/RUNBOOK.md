# The firmware build service on the build host

The build service (`build-service/`, the dev loop's builder: `docs/DEV_LOOP.md`) runs on the build host as the compose project `blinky-build`: the containers `blinky-build-api-1` and `blinky-build-worker-1`. The test box's `dev.py` and blinky-server's dev build path reach the api with `BLINKY_BUILD_URL` and `BLINKY_BUILD_TOKEN` from the box's `~/.config/blinky/build.env`.

- **Deploys.** The pull deployer (`deploy/build-host/deployer/`; installed by `deploy/build-host/labels/RUNBOOK.md`, "Install", step 7) runs `deploy.sh` for every gated master commit that changes `build-service/` or this directory. "Gated" means CI green (including `Build service`) and a clean Claude review. No GitHub runner is involved.
- **The sandbox.** `compose.yaml`'s header has the trust model. `check_config.py` refuses a config that loosens it, both in CI and in `deploy.sh` before every deploy. `verify.sh` checks the running containers.
- **Exposure.** The api is published on one private address of the host and one port, and nowhere else (no wildcard, not the LAN). `deploy.sh` refuses an address the host doesn't have. Nothing else fences it: there is no firewall unit, unlike the labels service, because nothing here reaches out except the api's mirror fetches from GitHub.
- **The token.** It lives in 1Password. `deploy.sh` reads it with `op read` through the host's 1Password service account, keeps it in its own environment for `docker compose up`, and never writes it to a file. The box holds the same value.
- **Host values.** Every host-specific value lives in `build.env` (gitignored; template `build.env.example`): the address, the port, the CPU set, the token's 1Password reference and the service-account token file. Not the box's file of the same name.

## Install (once per host)

1. **The deployer's clone** (`~/blinky-build-host`) and its timer, as for the labels service.
2. **`build.env`:** in that clone, copy `deploy/build-host/build/build.env.example` to `build.env` (mode 600) and fill it in. Moving an existing stack: use the address, port and CPU set it already runs with, so the box's `BLINKY_BUILD_URL` keeps working.
3. **The token:** a 1Password item with a field holding `openssl rand -hex 32` (the api refuses anything shorter than 32 characters), readable by the service account named in `build.env`.
4. **First deploy:** `./deploy.sh`, then `./verify.sh`: every line should say `ok`. The deployer's next pass after a merge that touches the service redeploys through the gate. Rerunning on an unchanged commit recreates nothing.
5. **The box:** `~/.config/blinky/build.env` (mode 600) gets `BLINKY_BUILD_URL=http://<address>:<port>` and `BLINKY_BUILD_TOKEN=<the token>`.

## Day to day

- **Deploys are automatic.** Merge to master; the deployer's next pass (every 2 minutes, after CI and the review are green) builds the images from the merged commit, waits up to 15 minutes for a build in flight to finish, recreates the containers and checks them. A failed deploy rolls back to the previous commit.
  - The deployer's log: `journalctl -u blinky-build-host-deploy`.
  - The deployer's state: `python3 deploy/build-host/deployer/deployer.py --status` (service `build`).
  - A refused commit: fix it on master, or `--accept SHA`.
- **"toolchain mismatch: rebuild the worker image".** The firmware's toolchain pin moved:
  1. Set `FIRMWARE_TOOLCHAIN_REF` in `compose.yaml` to a firmware commit on a branch with the new pin (and CI's `FIRMWARE_SHA` in `.github/workflows/pr-validation.yml`).
  2. If `arduino_cli` moved, bump `ARDUINO_CLI_VERSION`/`ARDUINO_CLI_SHA256` in `build-service/docker/Dockerfile.worker` too.
  3. Merge. The worker drops the old toolchain's build cache on start.
- **What's running:** `docker inspect -f '{{index .Config.Labels "org.opencontainers.image.revision"}}' blinky-build-api-1` is the tree hash of `build-service/` it was built from (`git rev-parse <commit>:build-service`).
- **Logs:**
  - service logs: `docker compose -p blinky-build logs api worker`;
  - one build's log: `GET /v1/builds/{id}` (`log_tail`), or `<id>/build.log` in the `blinky-build-jobs` volume.
- **State:** three named volumes:
  - `blinky-build-mirror`: the git mirror, rebuilt from GitHub if lost;
  - `blinky-build-jobs`: builds and packages; the newest 200 are kept;
  - `blinky-build-cache`: the warm compile cache, safe to delete.

  None of them is precious. The only secret is the token, and it is in 1Password.
- **Rotating the token:** update the 1Password item, run `./deploy.sh` by hand (a token change alone is not a commit, so the deployer won't do it), and update the box's `build.env`.
- **Turn it off:** `docker compose -p blinky-build stop`. A later deploy starts it again; to keep it off, also take `build` out of `deploy/build-host/deployer/services.json`.

## Retiring

1. Take `build` out of `deploy/build-host/deployer/services.json`.
2. `docker compose -p blinky-build down` (add `-v` to drop the volumes).
3. Remove `BLINKY_BUILD_URL` from the box's `build.env`; `dev.py` then fails, naming the key.
