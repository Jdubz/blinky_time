#!/usr/bin/env bash
# Build and (re)start the firmware build service from the blinky_time commit this checkout is at. The
# pull deployer (deploy/build-host/deployer) runs it for a gated master commit; it also works by hand.
# Refuses to start a config that loosens the sandbox (check_config.py) or binds anything but one of
# this host's own addresses. The bearer token is read from 1Password into this process's environment
# only; it is never written to a file. RUNBOOK.md.
set -euo pipefail
cd "$(dirname "$0")"
[ -f build.env ] || { echo "refusing: build.env missing (copy build.env.example, RUNBOOK.md)" >&2; exit 1; }
# shellcheck disable=SC1091
. ./build.env
: "${BLINKY_BUILD_BIND:?set in build.env} ${BLINKY_BUILD_PORT:?set in build.env} ${BLINKY_BUILD_CPUSET:?set in build.env}"
: "${BLINKY_BUILD_TOKEN_REF:?set in build.env} ${BLINKY_BUILD_OP_TOKEN_FILE:?set in build.env}"

# The version: the tree hash of build-service/ at this checkout (an image label; `docker inspect`).
BLINKY_BUILD_SERVICE_VERSION="$(git rev-parse HEAD:build-service)"
export BLINKY_BUILD_SERVICE_VERSION
compose() { docker compose --env-file build.env "$@"; }

# 1. The sandbox, checked on the config compose will actually run (with a placeholder token, so the
#    config read here holds no secret), and the bind address must be this host's.
read -r BIND PORT < <(BLINKY_BUILD_TOKEN=placeholder compose config --format json | python3 check_config.py)
ip -o addr show | grep -qw "inet $BIND" || { echo "refusing: $BIND is not an address of this host" >&2; exit 1; }

# 2. The token, from 1Password (the op service account's read scope).
OP_SERVICE_ACCOUNT_TOKEN="$(cat "$BLINKY_BUILD_OP_TOKEN_FILE")"
export OP_SERVICE_ACCOUNT_TOKEN
BLINKY_BUILD_TOKEN="$(op read --no-newline "$BLINKY_BUILD_TOKEN_REF")"
unset OP_SERVICE_ACCOUNT_TOKEN
[ "${#BLINKY_BUILD_TOKEN}" -ge 32 ] || { echo "refusing: the token from 1Password is shorter than 32 characters" >&2; exit 1; }
export BLINKY_BUILD_TOKEN

# 3. Build both images from this checkout while the running stack keeps serving. The worker image runs
#    blinky-firmware's setup_toolchain.sh at the pinned ref inside BuildKit (network for the downloads,
#    no host mounts, no secrets): several minutes the first time, cached afterwards.
compose build --pull

# 4. Let a build in flight finish before the worker is recreated (it would fail it; the box retries),
#    up to 15 minutes. A recreate only happens when the image or config changed.
worker=$(compose ps -q worker 2>/dev/null || true)
if [ -n "$worker" ]; then
  deadline=$((SECONDS + 900))
  while :; do
    busy=$(docker exec "$worker" python -c '
from blinky_build.jobs import JobStore
s = JobStore("/jobs")
print(sum((s.status(i) or {}).get("state") in ("queued", "building") for i in s.ids()))' 2>/dev/null || echo 0)
    [ "$busy" = 0 ] && break
    [ $SECONDS -lt $deadline ] || { echo "builds still running after 15 min: recreating anyway"; break; }
    echo "waiting for $busy build(s) in flight"; sleep 15
  done
fi

# 5. Start (`name: blinky-build` makes this a recreate in place on the same volumes).
compose up -d --no-build --remove-orphans

# 6. Both healthy, running the images just built, and the api answers on its address only.
deadline=$((SECONDS + 180))
for svc in api worker; do
  until [ "$(docker inspect -f '{{.State.Health.Status}}' "$(compose ps -q "$svc")" 2>/dev/null)" = healthy ]; do
    [ $SECONDS -lt $deadline ] || { echo "$svc did not become healthy" >&2; compose logs --tail 50 "$svc"; exit 1; }
    sleep 3
  done
  want=$(docker image inspect -f '{{.Id}}' "blinky-build-$svc:latest")
  got=$(docker inspect -f '{{.Image}}' "$(compose ps -q "$svc")")
  [ "$got" = "$want" ] || { echo "$svc runs image $got, not the one just built ($want)" >&2; exit 1; }
done
curl -fsS --max-time 10 "http://$BIND:$PORT/v1/healthz" >/dev/null || { echo "the api does not answer on its address" >&2; exit 1; }
ports=$(docker port "$(compose ps -q api)" | sort -u)
[ "$ports" = "$PORT/tcp -> $BIND:$PORT" ] || { echo "the api publishes: $ports" >&2; exit 1; }
echo "blinky-build deployed: build-service ${BLINKY_BUILD_SERVICE_VERSION:0:12}, both healthy, api on its private address only."
