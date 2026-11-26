#!/usr/bin/env bash
# The labels service must have NO open access to the host's network (owner, 2026-11-25).
#
#   sudo ./firewall.sh apply | check | remove | rules
#
# Persistent via blinky-labels-firewall.service (Before=docker.service, so the rules exist before any
# container starts); deploy.sh refuses to start the stack without them. The subnets come from
# labels.env next to this script.
#
# WHERE: the raw table's PREROUTING, not DOCKER-USER. On the build host, FORWARD jumps to
# tailscale's ts-forward BEFORE DOCKER-USER, and ts-forward ends in `-o tailscale0 -j ACCEPT`: a
# container's packet to a tailnet host is accepted there and never reaches DOCKER-USER. raw
# PREROUTING is the first hook a packet from a container bridge meets, both for forwarded traffic and
# for traffic to the host's own addresses (INPUT). Docker and tailscale only add DROPs to raw.
#
# WHAT: edge (internal, the service + the tunnel connector): traffic stays in the subnet. egress (the
# service only): TCP 443 to public addresses; every private, CGNAT/tailnet, link-local, loopback,
# multicast and reserved range is dropped first. DNS goes through Docker's embedded resolver.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
[ -f "$here/labels.env" ] || { echo "firewall.sh: $here/labels.env missing (labels.env.example)" >&2; exit 2; }
# shellcheck disable=SC1091
. "$here/labels.env"
: "${LABELS_FW_SOURCE:?} ${LABELS_EGRESS_SUBNET:?} ${LABELS_EGRESS_GATEWAY:?} ${LABELS_EDGE_SUBNET:?} ${LABELS_EDGE_GATEWAY:?}"

CHAIN=BLINKY-LABELS
# Generic reserved ranges (RFC 1918, RFC 6598, link-local, ...), not any host's address.
BLOCKED=(
  0.0.0.0/8
  10.0.0.0/8
  100.64.0.0/10
  127.0.0.0/8
  169.254.0.0/16
  172.16.0.0/12
  192.0.0.0/24
  192.0.2.0/24
  192.168.0.0/16
  198.18.0.0/15
  198.51.100.0/24
  203.0.113.0/24
  224.0.0.0/4
  240.0.0.0/4
)

ipt() { iptables -w 5 -t raw "$@"; }

rules() {
  echo "-s $LABELS_EDGE_SUBNET -d $LABELS_EDGE_GATEWAY -j DROP"
  echo "-s $LABELS_EDGE_SUBNET -d $LABELS_EDGE_SUBNET -j RETURN"
  echo "-s $LABELS_EDGE_SUBNET -j DROP"
  echo "-s $LABELS_EGRESS_SUBNET -d $LABELS_EGRESS_GATEWAY -j DROP"
  for net in "${BLOCKED[@]}"; do echo "-s $LABELS_EGRESS_SUBNET -d $net -j DROP"; done
  echo "-s $LABELS_EGRESS_SUBNET -p tcp -m tcp --dport 443 -j RETURN"
  echo "-s $LABELS_EGRESS_SUBNET -j DROP"
}

need_root() { [ "$(id -u)" = 0 ] || { echo "firewall.sh: run as root (sudo)" >&2; exit 2; }; }

apply() {
  need_root
  ipt -N "$CHAIN" 2>/dev/null || true
  ipt -F "$CHAIN"
  while read -r rule; do
    # shellcheck disable=SC2086  # word splitting of the rule is the point
    ipt -A "$CHAIN" $rule
  done < <(rules)
  ipt -C PREROUTING -s "$LABELS_FW_SOURCE" -j "$CHAIN" 2>/dev/null || ipt -I PREROUTING 1 -s "$LABELS_FW_SOURCE" -j "$CHAIN"
  echo "blinky-labels firewall applied: $(ipt -S "$CHAIN" | grep -c '^-A') rules in raw/$CHAIN"
}

check() {
  need_root
  local bad=0 want have
  ipt -C PREROUTING -s "$LABELS_FW_SOURCE" -j "$CHAIN" 2>/dev/null || { echo "MISSING: raw PREROUTING jump to $CHAIN"; bad=1; }
  while read -r rule; do
    # shellcheck disable=SC2086
    ipt -C "$CHAIN" $rule 2>/dev/null || { echo "MISSING: $CHAIN $rule"; bad=1; }
  done < <(rules)
  want=$(rules | wc -l)
  have=$(ipt -S "$CHAIN" 2>/dev/null | grep -c '^-A' || true)
  [ "$have" = "$want" ] || { echo "WRONG: $CHAIN has $have rules, expected $want"; bad=1; }
  if [ "$bad" = 0 ] && ! diff <(ipt -S "$CHAIN" | grep '^-A' | sed -E 's#/32##g; s/^-A [^ ]+ //') \
                             <(rules | sed -E 's#/32##g') >/dev/null; then
    echo "WRONG: $CHAIN rules are out of order"; bad=1
  fi
  [ "$bad" = 0 ] && echo "blinky-labels firewall OK ($want rules)"
  return "$bad"
}

remove() {
  need_root
  while ipt -C PREROUTING -s "$LABELS_FW_SOURCE" -j "$CHAIN" 2>/dev/null; do ipt -D PREROUTING -s "$LABELS_FW_SOURCE" -j "$CHAIN"; done
  ipt -F "$CHAIN" 2>/dev/null || true
  ipt -X "$CHAIN" 2>/dev/null || true
  echo "blinky-labels firewall removed"
}

case "${1:-}" in
  apply) apply ;;
  check) check ;;
  remove) remove ;;
  rules) rules ;;
  *) echo "usage: $0 apply|check|remove|rules" >&2; exit 2 ;;
esac
