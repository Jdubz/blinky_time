#!/usr/bin/env bash
# Prove the labels service has no open access to this network, from INSIDE its container. Run on the
# build host after deploy.sh, and after any firewall, network, tailscale or docker change:
#
#   ./verify.sh                  # every check; exit 0 only if all pass
#   ./verify.sh --public URL     # also: the API answers through the tunnel
#
# Targets are discovered here (this host's addresses, its gateway, tailnet peers, every bridge and
# container), never written down. A probe PASSES when the target is unreachable (timeout or no
# route); "connection refused" is a FAIL (a host answered). GitHub on 443 must connect.
# shellcheck disable=SC2015  # `ok` and `bad` only echo: A && ok || bad is a safe if-else here
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck disable=SC1091
. ./labels.env
C=blinky-labels
PUBLIC=""
[ "${1:-}" = --public ] && PUBLIC="${2:?--public needs the API URL}"
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

echo "== the container"
[ "$(docker inspect -f '{{.State.Health.Status}}' $C)" = healthy ] && ok "healthy" || bad "not healthy"
[ "$(docker inspect -f '{{.Config.User}}' $C)" = 10001:10001 ] && ok "runs as 10001" || bad "user"
[ "$(docker inspect -f '{{.HostConfig.ReadonlyRootfs}}' $C)" = true ] && ok "read-only root filesystem" || bad "rootfs writable"
[ "$(docker inspect -f '{{.HostConfig.CapDrop}}' $C)" = "[ALL]" ] && ok "cap_drop ALL" || bad "capabilities"
docker inspect -f '{{.HostConfig.SecurityOpt}}' $C | grep -q no-new-privileges && ok "no-new-privileges" || bad "no-new-privileges"
[ "$(docker inspect -f '{{json .HostConfig.PortBindings}}' $C)" = "{}" ] && [ -z "$(docker port $C)" ] \
  && ok "no published ports" || bad "published ports: $(docker port $C)"
nets=$(docker inspect -f '{{range $k, $v := .NetworkSettings.Networks}}{{$k}} {{end}}' $C)
[ "$nets" = "blinky-labels-edge blinky-labels-egress " ] && ok "networks: $nets" || bad "networks: $nets"
docker inspect -f '{{range .Mounts}}{{.Type}} {{end}}' $C | grep -qv bind && ok "no host bind mounts" || bad "a bind mount"
cg=$(docker exec $C cat /sys/fs/cgroup/cpuset.cpus.effective /sys/fs/cgroup/memory.max /sys/fs/cgroup/pids.max | tr '\n' ' ')
case "$cg" in "$LABELS_CPUSET "*) ok "fences: $cg";; *) bad "fences: $cg (cpuset must be $LABELS_CPUSET)";; esac
members=$(docker network inspect -f '{{range .Containers}}{{.Name}} {{end}}' blinky-labels-edge | tr ' ' '\n' | sort | xargs)
want=$(printf '%s\n' blinky-labels "$LABELS_TUNNEL_CONTAINER" | sort | xargs)
[ "$members" = "$want" ] && ok "edge network: $members" || bad "edge network members: '$members' (want '$want')"

echo "== the firewall"
systemctl is-enabled --quiet blinky-labels-firewall.service && ok "firewall unit enabled (survives reboot)" || bad "firewall unit not enabled"
sudo -n ./firewall.sh check >/dev/null && ok "firewall rules in place" || bad "firewall rules: $(sudo -n ./firewall.sh check || true)"

echo "== the deploy key (publishing)"
out=$(docker exec $C sh -c 'k=$(ls /tmp/deploy-key-* 2>/dev/null | head -1); [ -n "$k" ] || { echo NOKEY; exit 0; }
  ssh -i "$k" -o IdentitiesOnly=yes -o IdentityAgent=none -o BatchMode=yes -o StrictHostKeyChecking=yes \
      -o UserKnownHostsFile=/app/github_known_hosts -o GlobalKnownHostsFile=/dev/null -o ConnectTimeout=15 \
      -p 443 -T git@ssh.github.com 2>&1' || true)
case "$out" in
  *"Hi Jdubz/blinky-corpus!"*) ok "deploy key authenticates as Jdubz/blinky-corpus at ssh.github.com:443 (host key pinned)" ;;
  *NOKEY*) echo "note: no deploy key in the container: publishing is off" ;;
  *) bad "deploy key: $out" ;;
esac

echo "== probes from inside the container"
targets=()
for ip in $(ip -o -4 addr show scope global | awk '{print $4}' | cut -d/ -f1); do
  for port in 22 5432 9100 10050 8790 5002; do targets+=("$ip:$port this host ($ip)"); done
done
gw=$(ip route show default | awk '{print $3; exit}')
[ -n "$gw" ] && targets+=("$gw:80 the LAN gateway" "$gw:443 the LAN gateway (https)")
if command -v tailscale >/dev/null; then
  for ip in $(tailscale status --json | python3 -c 'import json,sys; d=json.load(sys.stdin); print(" ".join(p["TailscaleIPs"][0] for p in list((d.get("Peer") or {}).values())[:8] if p.get("TailscaleIPs")))'); do
    targets+=("$ip:22 tailnet peer" "$ip:443 tailnet peer (https)")
  done
fi
for ctr in $(docker ps --format '{{.Names}}' | grep -vx "$C"); do
  for ip in $(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' "$ctr"); do
    targets+=("$ip:80 container $ctr")
  done
done
for gwip in $(docker network ls -q | xargs docker network inspect -f '{{range .IPAM.Config}}{{.Gateway}} {{end}}'); do
  targets+=("$gwip:22 a docker bridge gateway (this host)")
done
targets+=("169.254.169.254:80 link-local metadata")
targets+=("ALLOW:api.github.com:443 GitHub API" "ALLOW:github.com:443 GitHub git" "ALLOW:ssh.github.com:443 GitHub ssh over 443"
          "github.com:80 GitHub on port 80 (only 443 is open)" "1.1.1.1:53 public DNS over TCP (only 443 is open)")
printf '%s\n' "${targets[@]}" | sort -u | docker exec -i $C python3 -c '
import socket, sys, errno
bad = n = failed = 0
for line in sys.stdin:
    spec, what = line.strip().split(" ", 1)
    allow = spec.startswith("ALLOW:")
    host, port = spec.removeprefix("ALLOW:").rsplit(":", 1)
    try:
        socket.create_connection((host, int(port)), timeout=4).close(); result, reached = "connected", True
    except ConnectionRefusedError:
        result, reached = "REFUSED (a host answered)", True
    except (TimeoutError, socket.timeout):
        result, reached = "timed out", False
    except OSError as e:
        result, reached = f"unreachable ({errno.errorcode.get(e.errno, e.errno)})", False
    good = reached if allow else not reached
    bad |= not good; n += 1; failed += not good
    if not good or allow:
        print(("ok   " if good else "FAIL ") + f"{what:42s} {host}:{port} -> {result}", flush=True)
print(f"{n} probes, {failed} not as expected" if bad else f"ok   all {n} probes as expected (everything private unreachable, GitHub on 443 reachable)")
sys.exit(1 if bad else 0)
' || fail=1

if [ -n "$PUBLIC" ]; then
  echo "== through the tunnel"
  for _ in 1 2 3 4 5 6; do curl -fsS --max-time 10 "$PUBLIC/healthz" 2>/dev/null | grep -q '"ok": true' && break; sleep 10; done
  curl -fsS --max-time 10 "$PUBLIC/healthz" | grep -q '"ok": true' && ok "$PUBLIC/healthz answers" || bad "$PUBLIC/healthz"
  o=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' -X OPTIONS -H "Origin: $LABELS_SITE_ORIGIN" \
      -H 'Access-Control-Request-Method: POST' "$PUBLIC/api/ops?stem=x" || true)
  [ "$o" = 204 ] && ok "CORS preflight for the site" || bad "CORS preflight: $o"
  o=$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' -X OPTIONS -H 'Origin: https://evil.example' \
      -H 'Access-Control-Request-Method: POST' "$PUBLIC/api/ops?stem=x" || true)
  [ "$o" = 403 ] && ok "CORS preflight refused for another origin" || bad "CORS for another origin: $o"
fi

echo
[ "$fail" = 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit "$fail"
