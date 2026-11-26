#!/usr/bin/env bash
# Build and (re)start the labels service from the blinky_time commit this checkout is at. The pull
# deployer (deploy/build-host/deployer) runs it for a gated master commit; it also works by hand.
# Refuses to start unless the network isolation is in place; renders the deploy key from 1Password
# (never persisted); nothing is published on a host port.
set -euo pipefail
cd "$(dirname "$0")"
[ -f labels.env ] || { echo "refusing: labels.env missing (copy labels.env.example, RUNBOOK.md)" >&2; exit 1; }

# 1. No open access to the network: the rules must be installed AND persistent before the container runs.
systemctl is-enabled --quiet blinky-labels-firewall.service || {
  echo "refusing: blinky-labels-firewall.service is not enabled (RUNBOOK.md)" >&2; exit 1; }
sudo -n ./firewall.sh apply
sudo -n ./firewall.sh check

# 2. The shared, internal edge network.
./network.sh

# 3. The deploy key, rendered to a transient file (the env is baked into the container at create).
: "${OP_SERVICE_ACCOUNT_TOKEN:=$(cat "$HOME/.config/op/spark-deploy-sa.token")}"
export OP_SERVICE_ACCOUNT_TOKEN
LABELS_ENV="$(mktemp)"; chmod 600 "$LABELS_ENV"
trap 'rm -f "$LABELS_ENV"' EXIT
if op item get "blinky labels corpus deploy key" --vault Development >/dev/null 2>&1; then
  op inject -f -i .env.labels.tpl -o "$LABELS_ENV"
  grep -q '^REVIEWER_DEPLOY_KEY_B64=.\{200,\}$' "$LABELS_ENV" || { echo "refusing: the rendered deploy key is empty or short" >&2; exit 1; }
else
  echo "WARNING: 1Password item 'blinky labels corpus deploy key' not found: publishing is OFF" >&2
  echo "REVIEWER_DEPLOY_KEY_B64=" > "$LABELS_ENV"
fi
export HOST_LABELS_ENV_FILE="$LABELS_ENV"
LABELS_SERVICE_VERSION="$(git rev-parse HEAD:labels-service)"
export LABELS_SERVICE_VERSION
compose() { docker compose --env-file labels.env "$@"; }

# 4. Nothing may be published; the sandbox fields must be there.
compose config --format json | python3 -c '
import json, sys
svc = json.load(sys.stdin)["services"]["reviewer"]
ports = svc.get("ports")
assert not ports, "refusing: the service publishes ports %r" % (ports,)
assert svc.get("read_only") and svc.get("cap_drop") == ["ALL"] and svc.get("user") == "10001:10001", "sandbox fields changed"
assert set(svc["networks"]) == {"edge", "egress"}, svc["networks"]'

# 5. Build from this checkout, start (the project name makes this a recreate in place).
compose build --pull
compose up -d

# 6. Healthy, the version it reports is this checkout's, and still nothing published.
deadline=$((SECONDS + 180))
until [ "$(docker inspect -f '{{.State.Health.Status}}' blinky-labels 2>/dev/null)" = healthy ]; do
  [ $SECONDS -lt $deadline ] || { echo "blinky-labels did not become healthy" >&2; compose logs --tail 50; exit 1; }
  sleep 3
done
got=$(docker exec blinky-labels python -c "import json,urllib.request; print(json.load(urllib.request.urlopen('http://127.0.0.1:8766/api/config'))['version'])")
[ "$got" = "$LABELS_SERVICE_VERSION" ] || { echo "blinky-labels reports version $got, expected $LABELS_SERVICE_VERSION" >&2; exit 1; }
[ -z "$(docker port blinky-labels)" ] || { echo "blinky-labels publishes a port: $(docker port blinky-labels)" >&2; exit 1; }
echo "blinky-labels deployed: version ${LABELS_SERVICE_VERSION:0:12}, healthy, no published ports, firewall in place."
