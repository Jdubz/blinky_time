#!/usr/bin/env bash
# Create (or check) blinky-labels-edge: the internal network the labels service shares with the
# tunnel connector, and nothing else. Neither stack owns it (a `down` of one must not pull it from
# the other); the connector's stack names it as external, so it must exist before that stack starts.
#   --internal   no route out (firewall.sh drops the rest)
#   labelled     the host's docker-gc prunes every network not labelled spark.protected
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck disable=SC1091
. "$here/labels.env"
: "${LABELS_EDGE_SUBNET:?} ${LABELS_EDGE_GATEWAY:?}"
NAME=blinky-labels-edge
if ! docker network inspect "$NAME" >/dev/null 2>&1; then
  docker network create --driver bridge --internal \
    --subnet "$LABELS_EDGE_SUBNET" --gateway "$LABELS_EDGE_GATEWAY" \
    --opt com.docker.network.bridge.name=br-blabels-edge \
    --label spark.protected=true --label blinky-labels=edge "$NAME" >/dev/null
  echo "created $NAME (internal)"
fi
got=$(docker network inspect -f '{{.Internal}} {{range .IPAM.Config}}{{.Subnet}}{{end}} {{index .Labels "spark.protected"}} {{.EnableIPv6}}' "$NAME")
[ "$got" = "true $LABELS_EDGE_SUBNET true false" ] || {
  echo "refusing: $NAME is '$got', expected 'true <LABELS_EDGE_SUBNET> true false'" >&2; exit 1; }
echo "$NAME OK (internal)"
